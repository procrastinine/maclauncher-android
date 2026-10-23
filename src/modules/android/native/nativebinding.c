#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "klepton.h"
#include "arm32/runtime.h"
#include <limits.h>
#include <sys/stat.h>
#include "native-invocation.h"
#include "native-image-provider.h"
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include "xr-surface.h"
#include "jni-lifetime.h"
#include "jni-invocation.h"
#include "device-context.h"

static pthread_mutex_t provider_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t loader_setup_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Atomic(const KljNativeImageProvider *) selected_provider;
static int image_loading_started;
int klj_native_image_provider_install(const KljNativeImageProvider *value,char *error,size_t capacity){
    if(!value||(value->size!=offsetof(KljNativeImageProvider,invoke)&&value->size!=offsetof(KljNativeImageProvider,jni_context)&&value->size<sizeof(*value))||value->version!=KLJ_NATIVE_IMAGE_PROVIDER_ABI||!value->owner||!value->load||!value->owns||!value->symbol||!value->native_abi||!value->register_exports||!value->initialize||!value->construct||!value->reject){if(error&&capacity)snprintf(error,capacity,"Invalid application native executor");return -EINVAL;}
    pthread_mutex_lock(&provider_mutex);
    if(image_loading_started||atomic_load_explicit(&selected_provider,memory_order_acquire)){pthread_mutex_unlock(&provider_mutex);if(error&&capacity)snprintf(error,capacity,"Application native execution is already initialized");return -EBUSY;}
    KljNativeImageProvider *copy=calloc(1,sizeof(*copy));
    if(!copy){pthread_mutex_unlock(&provider_mutex);if(error&&capacity)snprintf(error,capacity,"Application native executor could not be retained");return -ENOMEM;}
    memcpy(copy,value,value->size<sizeof(*copy)?value->size:sizeof(*copy));copy->size=sizeof(*copy);atomic_store_explicit(&selected_provider,copy,memory_order_release);pthread_mutex_unlock(&provider_mutex);if(error&&capacity)error[0]=0;return 0;
}
static const KljNativeImageProvider *begin_image_load(void){pthread_mutex_lock(&provider_mutex);image_loading_started=1;const KljNativeImageProvider *value=atomic_load_explicit(&selected_provider,memory_order_acquire);pthread_mutex_unlock(&provider_mutex);return value;}
static const KljNativeImageProvider *image_provider(jlong image){const KljNativeImageProvider *value=atomic_load_explicit(&selected_provider,memory_order_acquire);return value&&value->owns(value->owner,image)?value:NULL;}
int klj_native_image_invoke(JNIEnv *env,jlong image,KljCallPacket *packet){
    const KljNativeImageProvider *provider=image_provider(image);
    if(!provider)return 0;
    int64_t status=packet?-ENOSYS:-EINVAL;
    if(packet){
        if(packet->size!=sizeof(*packet)||packet->version!=KLJ_CALL_PACKET_VERSION)status=-EINVAL;
        else if(env&&(*env)->ExceptionCheck(env))status=-ECANCELED;
        else if(provider->invoke)status=provider->invoke(provider->owner,env,image,packet);
        packet->status=status;
    }
    if(status&&env&&!(*env)->ExceptionCheck(env)){
        jclass type=(*env)->FindClass(env,"java/lang/UnsatisfiedLinkError");
        if(type){(*env)->ThrowNew(env,type,"Android native callback invocation failed");(*env)->DeleteLocalRef(env,type);}
    }
    return 1;
}
int klj_native_image_jni_context(JNIEnv *env,jlong image,JNIEnv **guest_env,JavaVM **guest_vm){
    const KljNativeImageProvider *provider=image_provider(image);if(!provider)return 0;
    if(guest_env)*guest_env=NULL;if(guest_vm)*guest_vm=NULL;
    int64_t status=!env||!guest_env||!guest_vm?-EINVAL:!provider->jni_context?-ENOSYS:0;
    if(!status)status=(*env)->ExceptionCheck(env)?-ECANCELED:provider->jni_context(provider->owner,env,image,guest_env,guest_vm);
    if(status&&env&&!(*env)->ExceptionCheck(env)){
        jclass type=(*env)->FindClass(env,"java/lang/UnsatisfiedLinkError");
        if(type){(*env)->ThrowNew(env,type,"Android native Java context is unavailable");(*env)->DeleteLocalRef(env,type);}
    }
    return 1;
}

static kl_image *(*load_image)(const char *,const char *,int (*)(kl_image *,void *),void *);
static void *(*find_symbol)(kl_image *,const char *);
static int (*run_initializers)(kl_image *,int (*)(void *),void *);
static void (*reject_initialization)(kl_image *);
static const char *(*loader_error)(void);
static const kl_stats *(*image_stats)(kl_image *);
static const char *const *(*missing_imports)(kl_image *,unsigned *);
extern JavaVM *klj_guest_vm(JavaVM*);
extern void *klj_native_abi(void *,const char *,char *,size_t);
extern void klj_input_queue_install(JNIEnv*);
static void problem(JNIEnv *env,const char *message){if((*env)->ExceptionCheck(env))return;jclass c=(*env)->FindClass(env,"java/lang/IllegalStateException");if(c)(*env)->ThrowNew(env,c,message);}
static int initializer_failed(void *context){JNIEnv *env=context;return (*env)->ExceptionCheck(env);}
static int current_initializer_failed(void *context){JavaVM *vm=context;JNIEnv *env=NULL;return (*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)==JNI_OK && (*env)->ExceptionCheck(env);}
static void xr_surface_retire(JNIEnv *env,int phase){if(phase==KLJ_JNI_VM_DEATH){JavaVM *vm=NULL;klj_xr_surface_shutdown(env);klj_device_context_release(env);if((*env)->GetJavaVM(env,&vm)==JNI_OK)klj_jni_clear_vm(vm);}}
static KljJniVmObserver xr_surface_owner={.retire=xr_surface_retire};
static void observe_initializers(JNIEnv *env,void *runtime){
    extern void *klj_native_jni_import(const char *,void *);
    void (*imports)(void *(*)(const char *,void *))=dlsym(runtime,"klj_set_native_import_adapter");
    if(imports)imports(klj_native_jni_import);
    JavaVM *vm=NULL;
    void (*observe)(int (*)(void *),void *)=dlsym(runtime,"klj_set_initializer_observer");
    void (*thread_vm)(JavaVM *)=dlsym(runtime,"klj_set_guest_thread_vm");
    if((*env)->GetJavaVM(env,&vm)==JNI_OK){
        extern int klj_registration_prepare_facade(JNIEnv *);
        if(!klj_registration_prepare_facade(env))return;
        if(!klj_jni_publish_vm(vm,klj_guest_vm(vm))){problem(env,"Android native VM is already configured differently");return;}
        (void)klj_jni_vm_observe(env,&xr_surface_owner);
        void (*pressure)(void)=dlsym(runtime,"kl_mem_pressure_init");
        if(pressure)pressure();
        if(observe)observe(current_initializer_failed,vm);
        if(thread_vm)thread_vm(vm);
    }
}
static int construct(JNIEnv *env,kl_image *image){
    if(run_initializers(image,initializer_failed,env))return 1;
    if(!(*env)->ExceptionCheck(env))problem(env,"Android native library initialization previously failed");
    return 0;
}
/* Configured before any guest native code starts. The class monitor serializes
 * this entry; the retained runtime and its registered images live for the JVM. */
static void *configured_runtime;
static jint configured_target_sdk;
#include "arm32-components.h"
static char configured_runtime_path[PATH_MAX],configured_native_root[PATH_MAX];
static const klj_arm32_runtime_api *arm32_api;
static void *arm32_process;
static int arm32_ready;
static void *(*format_payload)(void *,unsigned);
static void *arm32_handle(jlong image){return format_payload?format_payload((void *)(uintptr_t)image,1):NULL;}
static int arm32_image(jlong image){return arm32_handle(image)!=NULL;}
static int elf32_file(const char *file){unsigned char header[5];FILE *input=fopen(file,"rb");if(!input)return 0;size_t count=fread(header,1,sizeof header,input);fclose(input);return count==sizeof header&&!memcmp(header,"\177ELF",4)&&header[4]==1;}
static int configure_arm32(JNIEnv *env,void *host,const char *runtime){
    if(arm32_ready)return 1;
    char library[PATH_MAX];const char *slash=strrchr(runtime,'/');
    if(!slash||(size_t)(slash-runtime)>=sizeof library){problem(env,"ARM32 execution library path is invalid");return 0;}
    if(!arm32_process){
    if(snprintf(library,sizeof library,"%.*s/%s",(int)(slash-runtime),runtime,KLJ_ARM32_JVM_LIBRARY)>=(int)sizeof library){problem(env,"ARM32 execution library path is invalid");return 0;}
    void *component=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!component){problem(env,dlerror());return 0;}
    const klj_arm32_runtime_api *(*api)(void)=dlsym(component,"klj_arm32_runtime");
    void *(*lookup)(const char*)=dlsym(host,"kl_shim_lookup");
    void *(*guest_symbol)(const char *)=dlsym(host,"kl_guest_sym_global");
    void (*register_format)(const KlImageFormat *)=dlsym(host,"klj_register_image_format");
    void (*set_elf_class)(unsigned)=dlsym(host,"klj_set_library_elf_class");
    format_payload=dlsym(host,"klj_image_format_payload");
    if(!api||!lookup||!guest_symbol||!register_format||!set_elf_class||!format_payload||(arm32_api=api())->version<2){dlclose(component);arm32_api=NULL;problem(env,"ARM32 native execution interface is unavailable");return 0;}
    arm32_process=arm32_api->create(env,lookup,guest_symbol);if(!arm32_process){dlclose(component);arm32_api=NULL;return 0;}
    register_format(arm32_api->format(arm32_process));
    /* The process owns one application native ABI. Platform dependencies keep
     * their shared native providers; only guest file selection changes width. */
    set_elf_class(1);
    }
    for(size_t i=0;i<sizeof klj_arm32_preloads/sizeof *klj_arm32_preloads;i++){
    if(snprintf(library,sizeof library,"%.*s/%s",(int)(slash-runtime),runtime,klj_arm32_preloads[i])>=(int)sizeof library){problem(env,"ARM32 execution library path is invalid");return 0;}
    kl_image *builtins=load_image(library,klj_arm32_preloads[i],NULL,NULL);
    if(!builtins){if(!(*env)->ExceptionCheck(env))problem(env,loader_error());return 0;}
    if(!construct(env,builtins))return 0;
    }
    arm32_ready=1;return 1;
}

/* The Java loader publishes this immutable runtime before Activity creation. */
void *klj_guest_runtime_symbol(const char *name){return configured_runtime?dlsym(configured_runtime,name):NULL;}
void *klj_guest_image_symbol(jlong image,const char *name){const KljNativeImageProvider *provider=image_provider(image);if(provider){char error[512];return provider->symbol(provider->owner,image,name,error,sizeof error);}return image&&!arm32_image(image)&&find_symbol?find_symbol((kl_image*)(uintptr_t)image,name):NULL;}
static int configured_path(JNIEnv *env,jbyteArray bytes,char *out,size_t capacity,int directory){
    if(!bytes){problem(env,"Native loader path is null");return 0;}
    jsize length=(*env)->GetArrayLength(env,bytes);
    if(length<=0||(size_t)length>=capacity){problem(env,"Native loader path exceeds its supported length");return 0;}
    (*env)->GetByteArrayRegion(env,bytes,0,length,(jbyte*)out);
    if((*env)->ExceptionCheck(env))return 0;
    out[length]=0;
    if(out[0]!='/'||memchr(out,0,(size_t)length)){problem(env,"Native loader path must be absolute and contain no NUL bytes");return 0;}
    char canonical[PATH_MAX];struct stat info;
    if(!realpath(out,canonical)||strcmp(out,canonical)||stat(out,&info)||
       (directory?!S_ISDIR(info.st_mode):!S_ISREG(info.st_mode))){
        problem(env,"Native loader path must name an existing canonical file or directory");return 0;
    }
    return 1;
}
static void startup_vector_free(char **values){if(values){for(size_t i=0;values[i];i++)free(values[i]);free(values);}}
static char *startup_utf8(JNIEnv *env,jstring value){
    if(!value){problem(env,"Android native startup contains a null string");return NULL;}
    jsize count=(*env)->GetStringLength(env,value);
    if(count>16384){problem(env,"Android native startup string is too long");return NULL;}
    const jchar *chars=(*env)->GetStringChars(env,value,NULL);if(!chars)return NULL;
    char *out=malloc((size_t)count*3+1);size_t at=0;const char *error=NULL;
    if(!out)error="Android native startup allocation failed";
    for(jsize i=0;out&&i<count;i++){
        uint32_t point=chars[i];
        if(point>=0xd800&&point<=0xdbff){
            if(i+1>=count||chars[i+1]<0xdc00||chars[i+1]>0xdfff){error="Android native startup contains an unpaired surrogate";break;}
            point=0x10000+((point-0xd800)<<10)+(chars[++i]-0xdc00);
        }else if(point>=0xdc00&&point<=0xdfff){error="Android native startup contains an unpaired surrogate";break;}
        if(!point){error="Android native startup contains a NUL character";break;}
        size_t bytes=point<0x80?1:point<0x800?2:point<0x10000?3:4;
        if(bytes==1)out[at++]=(char)point;
        else{out[at++]=(char)((bytes==2?0xc0:bytes==3?0xe0:0xf0)|(point>>(6*(bytes-1))));for(size_t shift=bytes-1;shift;shift--)out[at++]=(char)(0x80|((point>>(6*(shift-1)))&0x3f));}
    }
    (*env)->ReleaseStringChars(env,value,chars);
    if(error){free(out);problem(env,error);return NULL;}out[at]=0;return out;
}
static char **startup_vector(JNIEnv *env,jobjectArray array,jsize *count){
    if(!array){problem(env,"Android native startup is not configured");return NULL;}
    *count=(*env)->GetArrayLength(env,array);
    if(*count>64){problem(env,"Android native startup has too many entries");return NULL;}
    char **values=calloc((size_t)*count+1,sizeof(char *));
    if(!values){problem(env,"Android native startup allocation failed");return NULL;}
    for(jsize i=0;i<*count;i++){
        jstring value=(*env)->GetObjectArrayElement(env,array,i);
        if((*env)->ExceptionCheck(env)){startup_vector_free(values);return NULL;}
        values[i]=startup_utf8(env,value);if(value)(*env)->DeleteLocalRef(env,value);
        if(!values[i]){startup_vector_free(values);return NULL;}
    }
    return values;
}
static int application_startup(JNIEnv *env,int (*install)(int,const char *const *,size_t,const char *const *)){
    jclass context=(*env)->FindClass(env,"maclauncher/android/framework/HostContext");if(!context)return 0;
    jmethodID arguments=(*env)->GetStaticMethodID(env,context,"hostNativeStartupArguments","()[Ljava/lang/String;");
    jmethodID environment=arguments?(*env)->GetStaticMethodID(env,context,"hostNativeStartupEnvironment","()[Ljava/lang/String;"):NULL;
    jobjectArray argv=environment?(*env)->CallStaticObjectMethod(env,context,arguments):NULL;
    jobjectArray envp=!(*env)->ExceptionCheck(env)&&argv?(*env)->CallStaticObjectMethod(env,context,environment):NULL;
    jsize argc=0,envc=0;char **a=NULL,**e=NULL;
    if(!(*env)->ExceptionCheck(env)&&argv&&envp){a=startup_vector(env,argv,&argc);if(a)e=startup_vector(env,envp,&envc);}
    int ready=a&&e&&install(argc,(const char *const *)a,(size_t)envc,(const char *const *)e);
    startup_vector_free(a);startup_vector_free(e);
    if(argv)(*env)->DeleteLocalRef(env,argv);if(envp)(*env)->DeleteLocalRef(env,envp);(*env)->DeleteLocalRef(env,context);
    if(!ready&&!(*env)->ExceptionCheck(env))problem(env,"Android native process startup is unavailable or already initialized differently");
    return ready;
}
static jint application_target_sdk(JNIEnv *env){
    jclass context=(*env)->FindClass(env,"maclauncher/android/framework/HostContext");if(!context)return 0;
    jmethodID getter=(*env)->GetStaticMethodID(env,context,"hostApplicationTargetSdkVersion","()I");
    jint target=getter?(*env)->CallStaticIntMethod(env,context,getter):0;
    (*env)->DeleteLocalRef(env,context);return (*env)->ExceptionCheck(env)?0:target;
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeBinding_configureRuntime(JNIEnv *env,jclass cls,jbyteArray runtime,jbyteArray nativeRoot){
    char runtime_path[PATH_MAX],root[PATH_MAX];
    if(!configured_path(env,runtime,runtime_path,sizeof runtime_path,0)||
       !configured_path(env,nativeRoot,root,sizeof root,1))return;
    jint target=application_target_sdk(env);if((*env)->ExceptionCheck(env))return;
    if(target<=0){problem(env,"APK target SDK metadata is not configured");return;}
    if(configured_runtime){
        if(strcmp(configured_runtime_path,runtime_path)||(configured_native_root[0]&&strcmp(configured_native_root,root))){problem(env,"Native loader is already configured for a different runtime or directory");return;}
        if(configured_target_sdk&&configured_target_sdk!=target){problem(env,"Native loader is already configured for a different target SDK");return;}
    }
    void *handle=configured_runtime?configured_runtime:dlopen(runtime_path,RTLD_NOW|RTLD_GLOBAL);
    if(!handle){problem(env,dlerror());return;}
    void (*set_path)(const char*)=dlsym(handle,"kl_set_library_path");
    if(!set_path){if(!configured_runtime)dlclose(handle);problem(env,"Missing Klepton native library path export");return;}
    int (*set_target_sdk)(unsigned)=dlsym(handle,"klj_library_set_target_sdk");
    if(!set_target_sdk){if(!configured_runtime)dlclose(handle);problem(env,"Android native target SDK configuration is unavailable");return;}
    if(!set_target_sdk((unsigned)target)){if(!configured_runtime)dlclose(handle);problem(env,"Android native target SDK is already initialized for a different application");return;}
    int (*set_startup)(int,const char *const *,size_t,const char *const *)=dlsym(handle,"klj_library_set_startup");
    if(!set_startup){if(!configured_runtime)dlclose(handle);problem(env,"Android native process startup configuration is unavailable");return;}
    if(!application_startup(env,set_startup)){if(!configured_runtime)dlclose(handle);return;}
    if(configured_runtime&&configured_native_root[0])return;
    set_path(root);
    memcpy(configured_runtime_path,runtime_path,strlen(runtime_path)+1);
    memcpy(configured_native_root,root,strlen(root)+1);
    configured_runtime=handle;
    configured_target_sdk=target;
    observe_initializers(env,handle);
    klj_input_queue_install(env);
}
/* JNI modified UTF-8 encodes supplementary characters as two surrogate triples;
 * filesystem names require standard UTF-8. Refuse malformed/oversized input. */
static int filesystem_utf8(JNIEnv *env,jstring value,char *out,size_t capacity){
    if(!value){problem(env,"Native loader filename is null");return 0;}
    jsize count=(*env)->GetStringLength(env,value);
    if(count<=0||(size_t)count>=capacity){problem(env,"Native loader filename exceeds its supported length");return 0;}
    const jchar *chars=(*env)->GetStringChars(env,value,NULL);if(!chars)return 0;
    const char *error=NULL;size_t at=0;
    for(jsize i=0;i<count;i++){
        uint32_t point=chars[i];
        if(point>=0xd800&&point<=0xdbff){
            if(i+1>=count||chars[i+1]<0xdc00||chars[i+1]>0xdfff){error="Native loader filename contains an unpaired surrogate";break;}
            point=0x10000+((point-0xd800)<<10)+(chars[++i]-0xdc00);
        }else if(point>=0xdc00&&point<=0xdfff){error="Native loader filename contains an unpaired surrogate";break;}
        if(!point){error="Native loader filename contains a NUL character";break;}
        size_t bytes=point<0x80?1:point<0x800?2:point<0x10000?3:4;
        if(bytes>=capacity-at){error="Native loader filename exceeds its supported length";break;}
        if(bytes==1)out[at++]=(char)point;
        else{out[at++]=(char)((bytes==2?0xc0:bytes==3?0xe0:0xf0)|(point>>(6*(bytes-1))));for(size_t shift=bytes-1;shift;shift--)out[at++]=(char)(0x80|((point>>(6*(shift-1)))&0x3f));}
    }
    (*env)->ReleaseStringChars(env,value,chars);
    if(error){problem(env,error);return 0;}out[at]=0;return 1;
}
struct image_preparation {JNIEnv *env;void *runtime;const char *name;jclass entry;};
static int prepare_image(kl_image *guest,void *opaque){
    struct image_preparation *context=opaque;JNIEnv *env=context->env;void *host=context->runtime;const char *name=context->name;
    const kl_stats *st=image_stats(guest);
    fprintf(stderr,"Guest %s: imports=%u missing=%u TLS=%u/%u x18=%u/%u\n",name,st->imports_bound,st->imports_missing,st->tls_rewrites,st->tls_refused,st->x18_patched,st->x18_refused);
    if(st->imports_missing||st->tls_refused||st->x18_refused){
        char message[1024];size_t used=(size_t)snprintf(message,sizeof message,"Android native library has %u unresolved imports, %u unsupported TLS sites and %u unsupported register sites",st->imports_missing,st->tls_refused,st->x18_refused);
        unsigned count=0;const char *const *names=missing_imports?missing_imports(guest,&count):NULL;
        unsigned shown=0;
        for(;names&&shown<count&&shown<12;shown++){
            if(!names[shown])break;
            size_t length=strnlen(names[shown],161);
            if(length>160||used+length+4>=sizeof message)break;
            int written=snprintf(message+used,sizeof message-used,"%s%.*s",shown?", ":": ",(int)length,names[shown]);
            if(written<0)break;used+=(size_t)written;
        }
        if(shown<count&&used+5<sizeof message)snprintf(message+used,sizeof message-used," ...");
        if(st->tls_refused||st->x18_refused){
            problem(env,message);return 0;
        }
        // Unused imports already have Klepton's named lazy-call diagnostics.
        // They must not suppress constructors for an otherwise usable library.
        fprintf(stderr,"[android-native] %s: %s; unresolved calls retain their named diagnostic\n",name,message);
    }
    if(arm32_handle((jlong)(uintptr_t)guest))return 1; // JNI32 retains the same actual JVM owner with width-correct call gates.
    extern int klj_registration_prepare(JNIEnv*,void*,void*);
    extern int klj_class_lookup_bind(JNIEnv*,void*,void*,jclass);
    if(!klj_registration_prepare(env,host,guest))return 0;
    if(klj_class_lookup_bind(env,host,guest,context->entry))return 1;
    if(!(*env)->ExceptionCheck(env))problem(env,"Android native class loader ownership could not be retained");
    return 0;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeBinding_load(JNIEnv *env,jclass cls,jstring runtime,jstring filename,jstring soname){
    char runtime_path[PATH_MAX],file[PATH_MAX],name[256];
    if(!filesystem_utf8(env,runtime,runtime_path,sizeof runtime_path)||!filesystem_utf8(env,filename,file,sizeof file)||!filesystem_utf8(env,soname,name,sizeof name))return 0;
    if(configured_runtime&&strcmp(configured_runtime_path,runtime_path)){problem(env,"Native loader is already configured for a different runtime");return 0;}
    const KljNativeImageProvider *provider=begin_image_load();
    if(provider){extern _Thread_local int klj_skip_guest_onload;return provider->load(provider->owner,env,cls,file,name,NULL,!klj_skip_guest_onload);}
    pthread_mutex_lock(&loader_setup_mutex);
    void *host=configured_runtime?configured_runtime:dlopen(runtime_path,RTLD_NOW|RTLD_GLOBAL);
    if(!host){const char *error=dlerror();pthread_mutex_unlock(&loader_setup_mutex);problem(env,error);return 0;}
    if(!load_image){
        find_symbol=dlsym(host,"kl_sym");run_initializers=dlsym(host,"klj_run_initializers");
        reject_initialization=dlsym(host,"klj_reject_image_initialization");
        loader_error=dlsym(host,"klj_image_load_error");image_stats=dlsym(host,"kl_get_stats");
        missing_imports=dlsym(host,"kl_missing_imports");load_image=dlsym(host,"klj_load_image");
    }
    if(!load_image||!find_symbol||!run_initializers||!reject_initialization||!loader_error||!image_stats){if(!configured_runtime)dlclose(host);pthread_mutex_unlock(&loader_setup_mutex);problem(env,"Missing Klepton loader exports");return 0;}
    if(!configured_runtime){memcpy(configured_runtime_path,runtime_path,strlen(runtime_path)+1);configured_runtime=host;observe_initializers(env,host);}
    pthread_mutex_unlock(&loader_setup_mutex);
    if(elf32_file(file)&&!configure_arm32(env,host,runtime_path))return 0;
    struct image_preparation preparation={env,host,name,cls};
    kl_image *guest=load_image(file,name,prepare_image,&preparation);
    if(!guest){if(!(*env)->ExceptionCheck(env))problem(env,loader_error());return 0;}
    extern _Thread_local int klj_skip_guest_onload;
    extern void klj_initialize_guest(JNIEnv *,jlong);
    if(!klj_skip_guest_onload){if(!construct(env,guest))return 0;klj_initialize_guest(env,(jlong)(uintptr_t)guest);if((*env)->ExceptionCheck(env))return 0;}
    return (jlong)(uintptr_t)guest;
}
int klj_native_image_prepare(JNIEnv *env,jobject preparation,jlong image){
    if(!preparation||!image||(*env)->ExceptionCheck(env)){if(!(*env)->ExceptionCheck(env))problem(env,"Native image preparation is missing");return 0;}
    jclass type=(*env)->FindClass(env,"java/util/function/LongConsumer");
    if(!type)return 0;
    if(!(*env)->IsInstanceOf(env,preparation,type)){(*env)->DeleteLocalRef(env,type);problem(env,"Native image preparation is invalid");return 0;}
    jmethodID accept=(*env)->GetMethodID(env,type,"accept","(J)V");(*env)->DeleteLocalRef(env,type);
    if(!accept)return 0;
    (*env)->CallVoidMethod(env,preparation,accept,image);return !(*env)->ExceptionCheck(env);
}
jlong klj_load_prepared(JNIEnv *env,jclass cls,jstring runtime,jstring filename,jstring soname,jobject preparation){
    if(!preparation){problem(env,"Native image preparation is missing");return 0;}
    const KljNativeImageProvider *provider=begin_image_load();
    if(provider){
        char runtime_path[PATH_MAX],file[PATH_MAX],name[256];
        if(!filesystem_utf8(env,runtime,runtime_path,sizeof runtime_path)||!filesystem_utf8(env,filename,file,sizeof file)||!filesystem_utf8(env,soname,name,sizeof name))return 0;
        if(configured_runtime&&strcmp(configured_runtime_path,runtime_path)){problem(env,"Native loader is already configured for a different runtime");return 0;}
        return provider->load(provider->owner,env,cls,file,name,preparation,JNI_FALSE);
    }
    extern _Thread_local int klj_skip_guest_onload;int previous=klj_skip_guest_onload;klj_skip_guest_onload=1;
    jlong image=Java_maclauncher_android_bridge_NativeBinding_load(env,cls,runtime,filename,soname);klj_skip_guest_onload=previous;
    if(!image||(*env)->ExceptionCheck(env))return 0;
    if(!klj_native_image_prepare(env,preparation,image)){if(reject_initialization)reject_initialization((kl_image *)(uintptr_t)image);return 0;}
    if(!construct(env,(kl_image *)(uintptr_t)image))return 0;
    return image;
}
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_NativeBinding_register(JNIEnv *env,jclass cls,jlong image,jclass target,jobjectArray names,jobjectArray signatures,jobjectArray shortExports,jobjectArray longExports){
    (void)cls;
    const KljNativeImageProvider *provider=image_provider(image);
    if(!image||(!provider&&!arm32_image(image)&&!find_symbol)){problem(env,"Guest library is not loaded");return 0;}
    jobjectArray arrays[]={names,signatures,shortExports,longExports};
    for(int j=0;j<4;j++)if(!arrays[j]){problem(env,"Native method table is missing");return 0;}
    if(!target){problem(env,"Native method class is missing");return 0;}
    jsize n=(*env)->GetArrayLength(env,names);
    for(int j=1;j<4;j++)if(n!=(*env)->GetArrayLength(env,arrays[j])){problem(env,"Invalid native method table");return 0;}
    char **values=calloc((size_t)n*4,sizeof(char*));JNINativeMethod *methods=calloc(n,sizeof(*methods));
    if(n&&(!values||!methods)){free(values);free(methods);problem(env,"Native method allocation failed");return 0;}
    int failed=0;
    for(int j=0;j<4&&!failed;j++)for(jsize i=0;i<n;i++){
        jstring text=(*env)->GetObjectArrayElement(env,arrays[j],i);if((*env)->ExceptionCheck(env)||!text){failed=1;break;}
        const char *utf=(*env)->GetStringUTFChars(env,text,NULL);
        if(utf){values[(size_t)j*n+i]=strdup(utf);(*env)->ReleaseStringUTFChars(env,text,utf);}
        (*env)->DeleteLocalRef(env,text);if(!values[(size_t)j*n+i]){failed=1;break;}
    }
    jint result=0;
    if(!failed&&!provider&&arm32_image(image))result=arm32_api->bind(env,arm32_handle(image),target,(const char*const*)values,(const char*const*)(values+n),(const char*const*)(values+2*n),(const char*const*)(values+3*n),(size_t)n);
    else if(!failed){
        for(jsize i=0;i<n&&!failed;i++){
            methods[i].name=values[i];methods[i].signature=values[n+i];
            char message[1024];
            methods[i].fnPtr=provider?provider->symbol(provider->owner,image,values[2*n+i],message,sizeof message):find_symbol((kl_image*)(uintptr_t)image,values[2*n+i]);if(!methods[i].fnPtr)methods[i].fnPtr=provider?provider->symbol(provider->owner,image,values[3*n+i],message,sizeof message):find_symbol((kl_image*)(uintptr_t)image,values[3*n+i]);
            if(!methods[i].fnPtr){char message[1024];snprintf(message,sizeof message,"Missing JNI export: %s or %s",values[2*n+i],values[3*n+i]);problem(env,message);failed=1;break;}
            methods[i].fnPtr=provider?provider->native_abi(provider->owner,image,methods[i].fnPtr,methods[i].signature,message,sizeof message):klj_native_abi(methods[i].fnPtr,methods[i].signature,message,sizeof message);if(!methods[i].fnPtr){problem(env,message);failed=1;}
        }
        if(!failed)result=(provider?provider->register_exports(provider->owner,env,image,target,methods,n):(*env)->RegisterNatives(env,target,methods,n))==JNI_OK?n:0;
    }
    for(size_t i=0;i<(size_t)n*4;i++)free(values[i]);free(values);free(methods);
    if(failed&&!(*env)->ExceptionCheck(env))problem(env,"Native method registration failed");return result;
}

void klj_initialize_guest(JNIEnv *env,jlong image){const KljNativeImageProvider *provider=image_provider(image);if(provider){if(!provider->initialize(provider->owner,env,image)&&!(*env)->ExceptionCheck(env))problem(env,"Guest JNI_OnLoad failed");return;}if(arm32_image(image)){arm32_api->initialize(env,arm32_handle(image));return;}jint(*onload)(JavaVM*,void*)=find_symbol((kl_image*)(uintptr_t)image,"JNI_OnLoad");if(!onload)return;JavaVM *vm=NULL;if((*env)->GetJavaVM(env,&vm)!=JNI_OK)return;jint version=0;KLJ_INVOKE_GUEST(env,version=onload(klj_guest_vm(vm),NULL));if((*env)->ExceptionCheck(env))return;if(version<0)problem(env,"Guest JNI_OnLoad failed");}
void klj_construct_guest(JNIEnv *env,jlong image){const KljNativeImageProvider *provider=image_provider(image);if(provider){if(!provider->construct(provider->owner,env,image)&&!(*env)->ExceptionCheck(env))problem(env,"Android native library initialization failed");return;}construct(env,(kl_image*)(uintptr_t)image);}
void klj_reject_guest(jlong image){const KljNativeImageProvider *provider=image_provider(image);if(provider){provider->reject(provider->owner,image);return;}if(image&&reject_initialization)reject_initialization((kl_image*)(uintptr_t)image);}
