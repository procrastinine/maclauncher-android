#include <jni.h>
#include <jvmti.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* Native-created threads have no Java caller for HotSpot's FindClass. Keep the
 * defining loader with its actual ELF image instead of making every application
 * class visible through a process-wide fallback loader. Images live for the VM. */
typedef struct ImageLoader {
    uintptr_t base,end;
    jobject loader;
    int ambiguous;
    struct ImageLoader *next;
} ImageLoader;
static pthread_mutex_t lookup_mutex=PTHREAD_MUTEX_INITIALIZER;
static ImageLoader *image_loaders;
static jclass class_type,missing_type,not_found_type,out_of_memory_type;
static jmethodID class_loader,class_for_name,exception_cause;
static jclass (JNICALL *original_find)(JNIEnv *,const char *);
static jclass file_descriptor_type;
static jfieldID file_descriptor_value;
static jfieldID (JNICALL *original_field)(JNIEnv *,jclass,const char *,const char *);

static jfieldID JNICALL find_field(JNIEnv *env,jclass type,const char *name,const char *signature){
    if((*env)->ExceptionCheck(env))return NULL;
    /* Android and HotSpot expose the same owned POSIX descriptor using different
     * private field names. Return the real VM field ID, including for JNI32;
     * Get/SetIntField then continue to use the VM's existing object storage. */
    if(type&&name&&signature&&!strcmp(name,"descriptor")&&!strcmp(signature,"I")&&
       (*env)->IsSameObject(env,type,file_descriptor_type))return file_descriptor_value;
    return original_field(env,type,name,signature);
}

int klj_field_lookup_prepare(JNIEnv *env,jniNativeInterface *table){
    if(table->GetFieldID==find_field)return 1;
    if(!file_descriptor_type){
        jclass type=table->FindClass(env,"java/io/FileDescriptor");
        if(!type)return 0;
        jfieldID value=table->GetFieldID(env,type,"fd","I");
        if(value)file_descriptor_type=(*env)->NewGlobalRef(env,type);
        (*env)->DeleteLocalRef(env,type);
        if(!file_descriptor_type)return 0;
        file_descriptor_value=value;
    }
    original_field=table->GetFieldID;table->GetFieldID=find_field;return 1;
}

static int loader_for_address(JNIEnv *env,uintptr_t address,jobject *loader){
    int owned=0;*loader=NULL;
    pthread_mutex_lock(&lookup_mutex);
    for(ImageLoader *entry=image_loaders;entry;entry=entry->next){
        if(address>=entry->base&&address<entry->end){
            if(!entry->ambiguous){owned=1;*loader=(*env)->NewLocalRef(env,entry->loader);}
            break;
        }
    }
    pthread_mutex_unlock(&lookup_mutex);
    return owned;
}

jclass klj_find_from_image(JNIEnv *env,const char *name,uintptr_t caller){
    /* A pre-existing pending exception belongs to the caller. */
    if((*env)->ExceptionCheck(env))return NULL;
    jobject loader=NULL;
    if(!name||!loader_for_address(env,caller,&loader))return original_find(env,name);
    if((*env)->ExceptionCheck(env))return NULL;
    /* ART FindClass resolves but does not initialize. HotSpot FindClass does
     * initialize, so even trying it before the image's loader can run an Android
     * class initializer on the wrong native-loading thread. Host JNI callers
     * retain their VM's behavior; guest methods/fields retain normal JNI active
     * use initialization after this non-initializing class resolution. */
    jclass found=NULL;
    char *binary=strdup(name);
    if(!binary){(*env)->DeleteLocalRef(env,loader);(*env)->ThrowNew(env,out_of_memory_type,"JNI class name");return NULL;}
    for(char *at=binary;*at;at++)if(*at=='/')*at='.';
    jstring class_name=(*env)->NewStringUTF(env,binary);free(binary);
    if(class_name){
        jvalue arguments[3]={{.l=class_name},{.z=JNI_FALSE},{.l=loader}};
        found=(*env)->CallStaticObjectMethodA(env,class_type,class_for_name,arguments);
        if((*env)->ExceptionCheck(env)){
            jthrowable failure=(*env)->ExceptionOccurred(env);(*env)->ExceptionClear(env);
            /* FindClass reports NoClassDefFoundError, not Class.forName's
             * ClassNotFoundException. Preserve other linkage failures and the
             * original lookup exception as the cause of the JNI error. */
            int absent=(*env)->IsInstanceOf(env,failure,not_found_type);
            if(absent){
                (*env)->ThrowNew(env,missing_type,name);
                jthrowable missing=(*env)->ExceptionOccurred(env);(*env)->ExceptionClear(env);
                if(missing){
                    jvalue cause={.l=failure};
                    jobject value=(*env)->CallObjectMethodA(env,missing,exception_cause,&cause);
                    if(value)(*env)->DeleteLocalRef(env,value);
                    if(!(*env)->ExceptionCheck(env))(*env)->Throw(env,missing);
                    (*env)->DeleteLocalRef(env,missing);
                }
            }else (*env)->Throw(env,failure);
            (*env)->DeleteLocalRef(env,failure);
        }
        (*env)->DeleteLocalRef(env,class_name);
    }
    (*env)->DeleteLocalRef(env,loader);
    return found;
}

static jclass JNICALL find_from_image(JNIEnv *env,const char *name){return klj_find_from_image(env,name,(uintptr_t)__builtin_return_address(0));}

int klj_class_lookup_prepare(JNIEnv *env,jniNativeInterface *table){
    if(!class_type){
        jclass type=table->FindClass(env,"java/lang/Class");
        if(!type)return 0;
        class_loader=(*env)->GetMethodID(env,type,"getClassLoader","()Ljava/lang/ClassLoader;");
        if(!class_loader){(*env)->DeleteLocalRef(env,type);return 0;}
        class_for_name=(*env)->GetStaticMethodID(env,type,"forName","(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        if(!class_for_name){(*env)->DeleteLocalRef(env,type);return 0;}
        jclass missing=table->FindClass(env,"java/lang/NoClassDefFoundError");
        if(!missing){(*env)->DeleteLocalRef(env,type);return 0;}
        jclass retained_missing=(*env)->NewGlobalRef(env,missing);(*env)->DeleteLocalRef(env,missing);
        if(!retained_missing){(*env)->DeleteLocalRef(env,type);return 0;}
        jclass retained_type=(*env)->NewGlobalRef(env,type);(*env)->DeleteLocalRef(env,type);
        if(!retained_type){(*env)->DeleteGlobalRef(env,retained_missing);return 0;}
        jclass not_found=table->FindClass(env,"java/lang/ClassNotFoundException");
        jclass out_of_memory=not_found?table->FindClass(env,"java/lang/OutOfMemoryError"):NULL;
        jclass throwable=out_of_memory?table->FindClass(env,"java/lang/Throwable"):NULL;
        if(throwable)exception_cause=(*env)->GetMethodID(env,throwable,"initCause","(Ljava/lang/Throwable;)Ljava/lang/Throwable;");
        if(exception_cause)not_found_type=(*env)->NewGlobalRef(env,not_found);
        if(not_found_type)out_of_memory_type=(*env)->NewGlobalRef(env,out_of_memory);
        if(throwable)(*env)->DeleteLocalRef(env,throwable);
        if(out_of_memory)(*env)->DeleteLocalRef(env,out_of_memory);
        if(not_found)(*env)->DeleteLocalRef(env,not_found);
        if(!not_found_type||!out_of_memory_type){
            if(not_found_type)(*env)->DeleteGlobalRef(env,not_found_type);
            if(out_of_memory_type)(*env)->DeleteGlobalRef(env,out_of_memory_type);
            not_found_type=out_of_memory_type=NULL;
            (*env)->DeleteGlobalRef(env,retained_type);(*env)->DeleteGlobalRef(env,retained_missing);return 0;
        }
        missing_type=retained_missing;class_type=retained_type;
    }
    if(!klj_field_lookup_prepare(env,table))return 0;
    if(table->FindClass!=find_from_image){original_find=table->FindClass;table->FindClass=find_from_image;}
    return 1;
}

int klj_class_lookup_bind(JNIEnv *env,void *runtime,void *image,jclass entry_class){
    void *(*base)(void *)=dlsym(runtime,"kl_base");
    size_t (*span)(void *)=dlsym(runtime,"kl_span");
    uintptr_t start=base?(uintptr_t)base(image):0;size_t length=span?span(image):0;
    if(!class_type||!start||!length||length>UINTPTR_MAX-start)return 0;
    jobject loader=(*env)->CallObjectMethodA(env,entry_class,class_loader,NULL);
    if((*env)->ExceptionCheck(env))return 0;
    pthread_mutex_lock(&lookup_mutex);
    for(ImageLoader *entry=image_loaders;entry;entry=entry->next){
        if(entry->base!=start||entry->end!=start+length)continue;
        if(!(*env)->IsSameObject(env,entry->loader,loader))entry->ambiguous=1;
        pthread_mutex_unlock(&lookup_mutex);(*env)->DeleteLocalRef(env,loader);return 1;
    }
    ImageLoader *entry=calloc(1,sizeof(*entry));
    if(entry){
        entry->loader=loader?(*env)->NewGlobalRef(env,loader):NULL;
        if(entry->loader||!loader){entry->base=start;entry->end=start+length;entry->next=image_loaders;image_loaders=entry;}
        else{free(entry);entry=NULL;}
    }
    pthread_mutex_unlock(&lookup_mutex);(*env)->DeleteLocalRef(env,loader);
    return entry!=NULL;
}
