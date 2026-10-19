/* Original libnativeloader owns classloader references and namespace policy.
 * No host image or namespace representation participates in its handles. */
#include <stdbool.h>
static uint64_t native_loader_image;
static uint64_t invoke_original(JNIEnv *env,uint64_t image,const char *name,const klj_arm32_argument *arguments,size_t count,enum klj_arm32_jni_value result){
    uint64_t function=original_symbol(env,image,name);
    if(!function){if(!(*env)->ExceptionCheck(env))problem(env,"Original native loader export is unavailable");return 0;}
    return arm32_api->invoke_process(env,arm32_process,(uint32_t)function,arguments,count,result);
}
static void *view_original(JNIEnv *env,uint32_t address,size_t size,unsigned flags){
    KljProcessMemory *memory=&original_process.bootstrap.memory;
    unsigned permissions=((flags&4)?1:0)|((flags&2)?2:0)|((flags&1)?4:0);
    void *view=memory->memory(memory->context,address,size,permissions);
    if(!view)problem(env,"Native memory is unavailable");return view;
}
static uint64_t classloader_namespace(JNIEnv *env,jobject loader){
    if(!native_loader_image){problem(env,"Original native loader is not initialized");return 0;}
    if(configured_elf_class==1){
        const klj_arm32_argument arguments[]={{(uintptr_t)env,1,KLJ_ARM32_JNI_ENVIRONMENT,0,0},{(uintptr_t)loader,1,KLJ_ARM32_JNI_OBJECT,0,0}};
        return invoke_original(env,native_loader_image,"FindNamespaceByClassLoader",arguments,2,KLJ_ARM32_SCALAR);
    }
    void *(*find)(JNIEnv *,jobject)=(void*)(uintptr_t)original_symbol(env,native_loader_image,"FindNamespaceByClassLoader");
    if(!find){problem(env,"Original native namespace lookup export is unavailable");return 0;}
    return (uintptr_t)find(klj_guest_env(env),loader);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeBinding_prepareProcess(JNIEnv *env,jclass type,jstring runtime,jint width){
    (void)type;char file[PATH_MAX];
    if(width!=1&&width!=2){problem(env,"Invalid Android native library ABI");return;}
    if(!filesystem_utf8(env,runtime,file,sizeof file)||!runtime_host(env,file))return;
    if(configured_elf_class&&configured_elf_class!=(unsigned)width){problem(env,"Native process ABI is already configured");return;}
    configured_elf_class=(unsigned)width;
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeBinding_initializeNamespaces(JNIEnv *env,jclass type,jstring runtime,jint sdk,jstring executable){
    (void)executable;char root[PATH_MAX];
    if(!filesystem_utf8(env,runtime,root,sizeof root))return;
    if(!klj_native_process_loader()){problem(env,"Android native process is not initialized");return;}
    uint64_t setter=original_symbol(env,original_process.bootstrap.api.default_handle,"__loader_android_set_application_target_sdk_version");
    if(!setter){problem(env,"Original native loader target API is unavailable");return;}
    uint64_t argument=(uint32_t)sdk,result=0;KljProcessMemory *memory=&original_process.bootstrap.memory;
    int error=memory->call(memory->context,setter,&argument,1,&result);
    if(error){loader_problem(env,error);return;}
    native_loader_image=load_system_image(env,root,"libnativeloader.so",type);
    if(!native_loader_image)return;
    if(configured_elf_class==1)invoke_original(env,native_loader_image,"InitializeNativeLoader",NULL,0,KLJ_ARM32_SCALAR);
    else {
        void (*initialize)(void)=(void*)(uintptr_t)original_symbol(env,native_loader_image,"InitializeNativeLoader");
        if(!initialize){problem(env,"Original native loader initialization export is unavailable");return;}initialize();
    }
}
jstring klj_create_classloader_namespace(JNIEnv *env,jobject loader,jint sdk,jstring search,
        jstring permitted,jboolean shared,jstring dex_path,jstring sonames){
    if(!native_loader_image){problem(env,"Original native loader is not initialized");return NULL;}
    jlong image=(jlong)(uintptr_t)native_loader_image;
    if(configured_elf_class==1){
        const klj_arm32_argument arguments[]={
            {(uintptr_t)env,1,KLJ_ARM32_JNI_ENVIRONMENT,0,0},{(uint32_t)sdk,1,KLJ_ARM32_SCALAR,0,0},
            {(uintptr_t)loader,1,KLJ_ARM32_JNI_OBJECT,0,0},{shared,1,KLJ_ARM32_SCALAR,0,0},
            {(uintptr_t)dex_path,1,KLJ_ARM32_JNI_OBJECT,0,0},{(uintptr_t)search,1,KLJ_ARM32_JNI_OBJECT,0,0},
            {(uintptr_t)permitted,1,KLJ_ARM32_JNI_OBJECT,0,0},{(uintptr_t)sonames,1,KLJ_ARM32_JNI_OBJECT,0,0}};
        return (jstring)(uintptr_t)invoke_original(env,(uint64_t)image,"CreateClassLoaderNamespace",arguments,8,KLJ_ARM32_JNI_OBJECT);
    }
    jstring (*create)(JNIEnv *,int32_t,jobject,bool,jstring,jstring,jstring,jstring)=(void*)(uintptr_t)original_symbol(env,native_loader_image,"CreateClassLoaderNamespace");
    if(!create){problem(env,"Original classloader namespace export is unavailable");return NULL;}
    return create(klj_guest_env(env),sdk,loader,shared,dex_path,search,permitted,sonames);
}
JNIEXPORT jstring JNICALL Java_maclauncher_android_bridge_NativeBinding_createNamespace(JNIEnv *env,jclass type,
        jobject loader,jint sdk,jstring search,jstring permitted,jboolean shared,jstring dex_path,jstring sonames){
    (void)type;
    if(classloader_namespace(env,loader)||(*env)->ExceptionCheck(env))return NULL;
    return klj_create_classloader_namespace(env,loader,sdk,search,permitted,shared,dex_path,sonames);
}

/* Enumerate only direct handles retained by original soinfo. This traversal
 * creates the JVM's binding order, never a native dependency/load policy. */
struct image_closure {uint64_t *seen,*ordered;size_t count,capacity,ordered_count;};
static int collect_image(uint64_t image,struct image_closure *closure){
    for(size_t i=0;i<closure->count;i++)if(closure->seen[i]==image)return 1;
    if(closure->count==closure->capacity){
        size_t capacity=closure->capacity?closure->capacity*2:16;
        if(capacity<closure->capacity||capacity>INT_MAX||capacity>SIZE_MAX/sizeof(uint64_t))return 0;
        uint64_t *next=realloc(closure->seen,capacity*sizeof(uint64_t));if(!next)return 0;closure->seen=next;
        next=realloc(closure->ordered,capacity*sizeof(uint64_t));if(!next)return 0;closure->ordered=next;closure->capacity=capacity;
    }
    closure->seen[closure->count++]=image;
    KljLinkerImageInfo info={0};int error=klj_process_loader_inspect(&original_process,image,&info,NULL,0);
    if(error&&error!=ERANGE)return 0;
    if(info.children>SIZE_MAX/sizeof(uint64_t))return 0;
    uint64_t *children=info.children?malloc((size_t)info.children*sizeof(uint64_t)):NULL;
    if(info.children&&!children)return 0;
    error=klj_process_loader_inspect(&original_process,image,&info,children,(size_t)info.children);
    if(error){free(children);return 0;}
    for(size_t i=0;i<info.children;i++)if(!collect_image(children[i],closure)){free(children);return 0;}
    free(children);closure->ordered[closure->ordered_count++]=image;return 1;
}
JNIEXPORT jlongArray JNICALL Java_maclauncher_android_bridge_NativeBinding_imageClosure(JNIEnv *env,jclass type,jlong handle){
    (void)type;if(!handle||!klj_native_process_loader()){problem(env,"Native image dependency metadata is unavailable");return NULL;}
    struct image_closure closure={0};jlongArray result=NULL;
    if(!collect_image((uint64_t)handle,&closure))problem(env,"Native image dependency metadata is unavailable");
    else {
        result=(*env)->NewLongArray(env,(jsize)closure.ordered_count);
        if(result)(*env)->SetLongArrayRegion(env,result,0,(jsize)closure.ordered_count,(const jlong*)closure.ordered);
    }
    free(closure.seen);free(closure.ordered);return result;
}
JNIEXPORT jbyteArray JNICALL Java_maclauncher_android_bridge_NativeBinding_imagePath(JNIEnv *env,jclass type,jlong handle){
    (void)type;KljLinkerImageInfo info={0};char path[PATH_MAX];
    int error=klj_process_loader_inspect(&original_process,(uint64_t)handle,&info,NULL,0);
    if(error&&error!=ERANGE){loader_problem(env,error);return NULL;}
    error=klj_process_loader_string(&original_process,info.path,path,sizeof path);
    if(error){loader_problem(env,error);return NULL;}
    size_t count=strlen(path);jbyteArray result=(*env)->NewByteArray(env,(jsize)count);
    if(result)(*env)->SetByteArrayRegion(env,result,0,(jsize)count,(const jbyte*)path);return result;
}
struct native_open_call {JNIEnv *env;jobject loader;jint sdk;const char *file;jstring search;};
static void *open_native_library(void *opaque){
    struct native_open_call *call=opaque;JNIEnv *env=call->env;void *result=NULL;
    jlong image=(jlong)(uintptr_t)native_loader_image;bool bridged=false;
    if(configured_elf_class==1){
        uint32_t error=0;
        const klj_arm32_argument arguments[]={
            {(uintptr_t)env,1,KLJ_ARM32_JNI_ENVIRONMENT,0,0},{(uint32_t)call->sdk,1,KLJ_ARM32_SCALAR,0,0},
            {(uintptr_t)call->file,1,KLJ_ARM32_BUFFER,strlen(call->file)+1,4},{(uintptr_t)call->loader,1,KLJ_ARM32_JNI_OBJECT,0,0},
            {0,1,KLJ_ARM32_SCALAR,0,0},{(uintptr_t)call->search,1,KLJ_ARM32_JNI_OBJECT,0,0},
            {(uintptr_t)&bridged,1,KLJ_ARM32_BUFFER,sizeof bridged,6},{(uintptr_t)&error,1,KLJ_ARM32_BUFFER,sizeof error,6}};
        uint32_t handle=(uint32_t)invoke_original(env,(uint64_t)image,"OpenNativeLibrary",arguments,8,KLJ_ARM32_SCALAR);
        if((*env)->ExceptionCheck(env))return NULL;
        if(handle)result=(void*)(uintptr_t)handle;
        if(error){
            /* Borrowing the native error text does not transfer its allocation
             * owner. Read through the width adapter, then use the original free. */
            char message[1024];size_t n=0;
            while(n+1<sizeof message){const char *at=view_original(env,error+(uint32_t)n,1,4);if(!at)break;message[n++]=*at;if(!*at)break;}
            message[n<sizeof message?n:sizeof message-1]=0;
            jthrowable pending=(*env)->ExceptionOccurred(env);if(pending)(*env)->ExceptionClear(env);
            const klj_arm32_argument release={error,1,KLJ_ARM32_SCALAR,0,0};
            invoke_original(env,(uint64_t)image,"NativeLoaderFreeErrorMessage",&release,1,KLJ_ARM32_SCALAR);
            if(pending){(*env)->ExceptionClear(env);(*env)->Throw(env,pending);(*env)->DeleteLocalRef(env,pending);}
            else if(!result&&!(*env)->ExceptionCheck(env))problem(env,message);
        }
    }else{
        void *(*open)(JNIEnv *,int32_t,const char *,jobject,const char *,jstring,bool *,char **)=(void*)(uintptr_t)original_symbol(env,native_loader_image,"OpenNativeLibrary");
        void (*release)(char *)=(void*)(uintptr_t)original_symbol(env,native_loader_image,"NativeLoaderFreeErrorMessage");
        if(!open||!release){problem(env,"Original native library open exports are unavailable");return NULL;}
        char *error=NULL;result=open(klj_guest_env(env),call->sdk,call->file,call->loader,NULL,call->search,&bridged,&error);
        if(!result&&error&&!(*env)->ExceptionCheck(env))problem(env,error);if(error)release(error);
    }
    if(bridged&&!(*env)->ExceptionCheck(env)){problem(env,"Original native loader selected an unavailable native-bridge image representation");return NULL;}
    return result;
}
jlong klj_load_with_classloader(JNIEnv *env,jclass entry,jobject loader,jint sdk,jstring filename,jstring search){
    char file[PATH_MAX];if(!filesystem_utf8(env,filename,file,sizeof file))return 0;
    if(!native_loader_image){problem(env,"Original native loader is not initialized");return 0;}
    extern int klj_registration_prepare_original(JNIEnv*);
    if(!klj_registration_prepare_original(env))return 0;
    KljJniInvocation scope __attribute__((cleanup(klj_jni_invocation_leave)));
    if(!klj_jni_invocation_enter(&scope,env))return 0;
    struct native_open_call call={env,loader,sdk,file,search};
    uint64_t image=(uintptr_t)open_native_library(&call);
    if(!image){if(!(*env)->ExceptionCheck(env))loader_problem(env,0);return 0;}
    return prepare_image(env,image,entry)?(jlong)image:0;
}
