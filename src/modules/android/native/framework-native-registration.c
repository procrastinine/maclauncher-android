#include <jni.h>
#include <jvmti.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "android-log.h"
#include "android-log-abi.h"

/* The publisher owns its complete registration tables. Concrete Mac JNI
 * exports and already-implemented Java boundaries take precedence per method;
 * an unimplemented method is never removed because of its class or family. */
static _Atomic(void *) publisher_base;
static void *publisher_library,*platform_library;
/* The supplied native component has its own host-resource configuration entry.
 * Retain its original ABI during registration, without replacing property APIs
 * or introducing another property dictionary. */
static void (*publisher_set_property)(JNIEnv *,jclass,jstring,jstring);

/* The complete publisher still owns priorities, buffers, filtering and JNI.
 * Its original logger callback only selects the existing physical destination. */
static void publisher_log(const struct __android_log_message *message){
    int saved=errno;
    if(message&&message->struct_size>=offsetof(struct __android_log_message,message)+sizeof(message->message))
        klj_android_log_write(message->priority,message->tag,message->message);
    errno=saved;
}

static void fail(JNIEnv *env,const char *message){
    if((*env)->ExceptionCheck(env))return;
    jclass type=(*env)->FindClass(env,"java/lang/UnsatisfiedLinkError");
    if(type){(*env)->ThrowNew(env,type,message);(*env)->DeleteLocalRef(env,type);}
}

/* JNI symbols encode UTF-16 units; JVMTI names use modified UTF-8, including
 * surrogate pairs as two triples. The caller reserves six bytes per input byte. */
static char *mangle(char *out,const char *in,size_t length){
    for(size_t i=0;i<length;){
        unsigned c=(unsigned char)in[i++];
        if(c>=0xc0&&c<0xe0&&i<length){c=((c&31)<<6)|((unsigned char)in[i++]&63);}
        else if(c>=0xe0&&i+1<length){unsigned a=(unsigned char)in[i++],b=(unsigned char)in[i++];c=((c&15)<<12)|((a&63)<<6)|(b&63);}
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9'))*out++=(char)c;
        else if(c=='/')*out++='_';
        else if(c=='_'||c==';'||c=='['){*out++='_';*out++=c=='_'?'1':c==';'?'2':'3';}
        else{snprintf(out,7,"_0%04x",c);out+=6;}
    }
    *out=0;return out;
}

static void *platform_entry(JNIEnv *env,const char *owner,const char *name,const char *descriptor){
    size_t owner_size=strlen(owner),name_size=strlen(name),signature_size=strlen(descriptor);
    if(owner_size<2||owner[0]!='L'||owner[owner_size-1]!=';')return NULL;
    if(owner_size>SIZE_MAX/6-name_size-signature_size-32){fail(env,"Android native symbol is too large");return NULL;}
    char *symbol=malloc(6*(owner_size+name_size+signature_size)+32);
    if(!symbol){fail(env,"Cannot allocate Android native symbol");return NULL;}
    memcpy(symbol,"Java_",5);char *end=mangle(symbol+5,owner+1,owner_size-2);
    *end++='_';end=mangle(end,name,name_size);
    void *result=dlsym(platform_library,symbol);
    if(!result&&descriptor[0]=='('){
        const char *close=strchr(descriptor,')');
        if(close){*end++='_';*end++='_';mangle(end,descriptor+1,(size_t)(close-descriptor-1));result=dlsym(platform_library,symbol);}
    }
    free(symbol);return result;
}

/* A registration reads one complete class snapshot. Index its declarations
 * once instead of asking JVMTI to allocate every method name again for each
 * publisher entry. Nothing is retained across classes or class-loader lives. */
typedef struct Declaration {char *name,*descriptor;jint modifiers;} Declaration;
static int declaration_order(const void *left,const void *right){
    const Declaration *a=left,*b=right;int name=strcmp(a->name,b->name);
    return name?name:strcmp(a->descriptor,b->descriptor);
}
static void declarations_free(jvmtiEnv *ti,Declaration *entries,jint count){
    if(!entries)return;
    for(jint i=0;i<count;i++){
        if(entries[i].name)(*ti)->Deallocate(ti,(unsigned char*)entries[i].name);
        if(entries[i].descriptor)(*ti)->Deallocate(ti,(unsigned char*)entries[i].descriptor);
    }
    free(entries);
}

/* Called by the one JNI registration dispatcher. NULL output means that this
 * is another library's table and its ordinary registration remains untouched. */
int klj_framework_registration(JNIEnv *env,jclass target,const JNINativeMethod *input,jint count,JNINativeMethod **output,jint *size){
    *output=NULL;*size=count;
    void *base=atomic_load_explicit(&publisher_base,memory_order_acquire);if(!base)return 0;
    int supplied=0;
    for(jint i=0;i<count;i++){Dl_info info={0};if(input[i].fnPtr&&dladdr(input[i].fnPtr,&info)&&info.dli_fbase==base){supplied=1;break;}}
    if(!supplied)return 0;
    JavaVM *vm=NULL;jvmtiEnv *ti=NULL;jmethodID *methods=NULL;jint method_count=0;char *owner=NULL;
    Declaration *declarations=NULL;
    JNINativeMethod *selected=NULL;
    if((*env)->GetJavaVM(env,&vm)!=JNI_OK||(*vm)->GetEnv(vm,(void**)&ti,JVMTI_VERSION_1_2)!=JNI_OK||
       (*ti)->GetClassSignature(ti,target,&owner,NULL)!=JVMTI_ERROR_NONE||
       (*ti)->GetClassMethods(ti,target,&method_count,&methods)!=JVMTI_ERROR_NONE){fail(env,"Cannot read Android native declarations");goto failed;}
    declarations=method_count?calloc((size_t)method_count,sizeof(*declarations)):NULL;
    if(method_count&&!declarations){fail(env,"Cannot allocate Android method identities");goto failed;}
    for(jint j=0;j<method_count;j++){
        Declaration *entry=&declarations[j];
        if((*ti)->GetMethodName(ti,methods[j],&entry->name,&entry->descriptor,NULL)!=JVMTI_ERROR_NONE||!entry->name||!entry->descriptor){fail(env,"Cannot read Android native method identity");goto failed;}
        if((*ti)->GetMethodModifiers(ti,methods[j],&entry->modifiers)!=JVMTI_ERROR_NONE){fail(env,"Cannot read Android native method modifiers");goto failed;}
    }
    if(method_count>1)qsort(declarations,(size_t)method_count,sizeof(*declarations),declaration_order);
    if(count<0||method_count>INT_MAX-count){fail(env,"Android native registration table is too large");goto failed;}
    size_t capacity=(size_t)count+(size_t)method_count;
    if(capacity>SIZE_MAX/sizeof(*selected)){fail(env,"Android native registration table is too large");goto failed;}
    size_t allocation=capacity*sizeof(*selected);
    for(jint j=0;j<method_count;j++)if(declarations[j].modifiers&0x100){
        size_t name_size=strlen(declarations[j].name)+1,signature_size=strlen(declarations[j].descriptor)+1;
        if(name_size>SIZE_MAX-allocation||signature_size>SIZE_MAX-allocation-name_size){fail(env,"Android native method identities are too large");goto failed;}
        allocation+=name_size+signature_size;
    }
    selected=allocation?malloc(allocation):NULL;
    if(allocation&&!selected){fail(env,"Cannot allocate Android native registration table");goto failed;}
    char *identities=selected?(char*)(selected+capacity):NULL;
    *size=0;
    for(jint i=0;i<count;i++){
        int implemented=0;Dl_info image={0};
        int publisher=input[i].fnPtr&&dladdr(input[i].fnPtr,&image)&&image.dli_fbase==base;
        if(publisher&&!strcmp(owner,"Landroid/os/SystemProperties;")&&!strcmp(input[i].name,"native_set")&&!strcmp(input[i].signature,"(Ljava/lang/String;Ljava/lang/String;)V"))
            publisher_set_property=(void (*)(JNIEnv *,jclass,jstring,jstring))input[i].fnPtr;
        if(publisher&&method_count){
            Declaration key={input[i].name,input[i].signature,0};
            Declaration *match=bsearch(&key,declarations,(size_t)method_count,sizeof(*declarations),declaration_order);
            implemented=match&&!(match->modifiers&0x100);
        }
        if(implemented)continue;
        selected[*size]=input[i];
        if(publisher){
            void *owned=platform_entry(env,owner,input[i].name,input[i].signature);
            if((*env)->ExceptionCheck(env))goto failed;
            if(owned)selected[*size].fnPtr=owned;
        }
        ++*size;
    }
    /* Host publisher builds omit device-only table rows even when the complete
     * framework retains the original declaration. Bind any concrete platform
     * export for those rows, using the same exact class/name/descriptor lookup
     * as replacements. The table owns its copied identities until dispatch
     * completes; no class/member policy or second registration owner is added. */
    for(jint j=0;j<method_count;j++){
        Declaration *entry=&declarations[j];if(!(entry->modifiers&0x100))continue;
        int supplied_entry=0;
        for(jint i=0;i<count;i++)if(!strcmp(input[i].name,entry->name)&&!strcmp(input[i].signature,entry->descriptor)){supplied_entry=1;break;}
        if(supplied_entry)continue;
        void *owned=platform_entry(env,owner,entry->name,entry->descriptor);
        if((*env)->ExceptionCheck(env))goto failed;
        if(!owned)continue;
        size_t name_size=strlen(entry->name)+1,signature_size=strlen(entry->descriptor)+1;
        selected[*size].name=identities;memcpy(identities,entry->name,name_size);identities+=name_size;
        selected[*size].signature=identities;memcpy(identities,entry->descriptor,signature_size);identities+=signature_size;
        selected[*size].fnPtr=owned;++*size;
    }
    declarations_free(ti,declarations,method_count);
    if(methods)(*ti)->Deallocate(ti,(unsigned char*)methods);
    if(owner)(*ti)->Deallocate(ti,(unsigned char*)owner);
    (*ti)->DisposeEnvironment(ti);*output=selected;return 1;
failed:
    if(ti){declarations_free(ti,declarations,method_count);if(methods)(*ti)->Deallocate(ti,(unsigned char*)methods);if(owner)(*ti)->Deallocate(ti,(unsigned char*)owner);(*ti)->DisposeEnvironment(ti);}
    free(selected);return -1;
}

JNIEXPORT void JNICALL Java_maclauncher_android_framework_JvmNativeMemory_configureResourceDirectory(JNIEnv *env,jclass type,jstring directory){
    (void)type;
    if(!publisher_set_property){fail(env,"Android native resource configuration is unavailable");return;}
    jclass properties=(*env)->FindClass(env,"android/os/SystemProperties");
    if(!properties)return;
    jstring key=(*env)->NewStringUTF(env,"ro.hyphen.data.dir");
    if(key){publisher_set_property(env,properties,key,directory);(*env)->DeleteLocalRef(env,key);}
    (*env)->DeleteLocalRef(env,properties);
}

extern int klj_registration_install(JNIEnv *env);
JNIEXPORT void JNICALL Java_maclauncher_android_framework_JvmNativeMemory_completeRegistration(JNIEnv *env,jclass type){
    (void)env;(void)type;
    /* The publisher's JNI_OnLoad initializes libbase logging and selects its
     * default logger. Publish the physical destination after that original
     * initialization, without replacing its registration or log filtering. */
    if(!publisher_library)return;
    void (*set_logger)(__android_logger_function)=dlsym(publisher_library,"__android_log_set_logger");
    if(set_logger)set_logger(publisher_log);
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_JvmNativeMemory_prepareRegistration(JNIEnv *env,jclass type,jbyteArray bytes){
    (void)type;if(atomic_load_explicit(&publisher_base,memory_order_acquire))return;
    jsize length=(*env)->GetArrayLength(env,bytes);char *file=malloc((size_t)length+1);
    if(!file){fail(env,"Cannot allocate Android native library path");return;}
    (*env)->GetByteArrayRegion(env,bytes,0,length,(jbyte*)file);file[length]=0;
    if((*env)->ExceptionCheck(env)){free(file);return;}
    if(!length||memchr(file,0,(size_t)length)){free(file);fail(env,"Invalid Android native library path");return;}
    void *library=dlopen(file,RTLD_LAZY|RTLD_LOCAL);free(file);
    if(!library){fail(env,dlerror());return;}
    klj_android_log_initialize();
    Dl_info publisher={0},platform={0};
    if(!dladdr(dlsym(library,"JNI_OnLoad"),&publisher)||!dladdr((void*)klj_framework_registration,&platform)){
        dlclose(library);fail(env,"Android native registration images are unavailable");return;
    }
    void *owned=dlopen(platform.dli_fname,RTLD_LAZY|RTLD_LOCAL);
    if(!owned){dlclose(library);fail(env,dlerror());return;}
    if(!klj_registration_install(env)){dlclose(owned);dlclose(library);return;}
    /* These JNI code owners remain loaded for the same JVM lifetime as their
     * registered function pointers. No per-game callback owns these handles. */
    publisher_library=library;platform_library=owned;
    atomic_store_explicit(&publisher_base,publisher.dli_fbase,memory_order_release);
}
