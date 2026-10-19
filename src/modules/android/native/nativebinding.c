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
static void observe_initializers(JNIEnv *env,void *runtime){
    extern void *klj_native_jni_import(const char *,void *);
    void (*imports)(void *(*)(const char *,void *))=dlsym(runtime,"klj_set_native_import_adapter");
    if(imports)imports(klj_native_jni_import);
    JavaVM *vm=NULL;
    void (*observe)(int (*)(void *),void *)=dlsym(runtime,"klj_set_initializer_observer");
    void (*thread_vm)(JavaVM *)=dlsym(runtime,"klj_set_guest_thread_vm");
    if((*env)->GetJavaVM(env,&vm)==JNI_OK){
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
void *klj_guest_image_symbol(jlong image,const char *name){return image&&!arm32_image(image)&&find_symbol?find_symbol((kl_image*)(uintptr_t)image,name):NULL;}
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
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeBinding_configureRuntime(JNIEnv *env,jclass cls,jbyteArray runtime,jbyteArray nativeRoot){
    char runtime_path[PATH_MAX],root[PATH_MAX];
    if(!configured_path(env,runtime,runtime_path,sizeof runtime_path,0)||
       !configured_path(env,nativeRoot,root,sizeof root,1))return;
    if(configured_runtime){
        if(strcmp(configured_runtime_path,runtime_path)||(configured_native_root[0]&&strcmp(configured_native_root,root))){problem(env,"Native loader is already configured for a different runtime or directory");return;}
        if(configured_native_root[0])return;
    }
    void *handle=configured_runtime?configured_runtime:dlopen(runtime_path,RTLD_NOW|RTLD_GLOBAL);
    if(!handle){problem(env,dlerror());return;}
    void (*set_path)(const char*)=dlsym(handle,"kl_set_library_path");
    if(!set_path){if(!configured_runtime)dlclose(handle);problem(env,"Missing Klepton native library path export");return;}
    set_path(root);
    memcpy(configured_runtime_path,runtime_path,strlen(runtime_path)+1);
    memcpy(configured_native_root,root,strlen(root)+1);
    configured_runtime=handle;
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
    void *host=configured_runtime?configured_runtime:dlopen(runtime_path,RTLD_NOW|RTLD_GLOBAL);
    if(!host){problem(env,dlerror());return 0;}
    if(!load_image){
        find_symbol=dlsym(host,"kl_sym");run_initializers=dlsym(host,"klj_run_initializers");
        reject_initialization=dlsym(host,"klj_reject_image_initialization");
        loader_error=dlsym(host,"klj_image_load_error");image_stats=dlsym(host,"kl_get_stats");
        missing_imports=dlsym(host,"kl_missing_imports");load_image=dlsym(host,"klj_load_image");
    }
    if(!load_image||!find_symbol||!run_initializers||!reject_initialization||!loader_error||!image_stats){if(!configured_runtime)dlclose(host);problem(env,"Missing Klepton loader exports");return 0;}
    if(!configured_runtime){memcpy(configured_runtime_path,runtime_path,strlen(runtime_path)+1);configured_runtime=host;observe_initializers(env,host);}
    if(elf32_file(file)&&!configure_arm32(env,host,runtime_path))return 0;
    struct image_preparation preparation={env,host,name,cls};
    kl_image *guest=load_image(file,name,prepare_image,&preparation);
    if(!guest){if(!(*env)->ExceptionCheck(env))problem(env,loader_error());return 0;}
    extern _Thread_local int klj_skip_guest_onload;
    extern void klj_initialize_guest(JNIEnv *,jlong);
    if(!klj_skip_guest_onload){if(!construct(env,guest))return 0;klj_initialize_guest(env,(jlong)(uintptr_t)guest);if((*env)->ExceptionCheck(env))return 0;}
    return (jlong)(uintptr_t)guest;
}
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_NativeBinding_register(JNIEnv *env,jclass cls,jlong image,jclass target,jobjectArray names,jobjectArray signatures,jobjectArray shortExports,jobjectArray longExports){
    (void)cls;
    if(!image||(!arm32_image(image)&&!find_symbol)){problem(env,"Guest library is not loaded");return 0;}
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
    if(!failed&&arm32_image(image))result=arm32_api->bind(env,arm32_handle(image),target,(const char*const*)values,(const char*const*)(values+n),(const char*const*)(values+2*n),(const char*const*)(values+3*n),(size_t)n);
    else if(!failed){
        for(jsize i=0;i<n&&!failed;i++){
            methods[i].name=values[i];methods[i].signature=values[n+i];
            methods[i].fnPtr=find_symbol((kl_image*)(uintptr_t)image,values[2*n+i]);if(!methods[i].fnPtr)methods[i].fnPtr=find_symbol((kl_image*)(uintptr_t)image,values[3*n+i]);
            if(!methods[i].fnPtr){char message[1024];snprintf(message,sizeof message,"Missing JNI export: %s or %s",values[2*n+i],values[3*n+i]);problem(env,message);failed=1;break;}
            char message[1024];methods[i].fnPtr=klj_native_abi(methods[i].fnPtr,methods[i].signature,message,sizeof message);if(!methods[i].fnPtr){problem(env,message);failed=1;}
        }
        if(!failed)result=(*env)->RegisterNatives(env,target,methods,n)==JNI_OK?n:0;
    }
    for(size_t i=0;i<(size_t)n*4;i++)free(values[i]);free(values);free(methods);
    if(failed&&!(*env)->ExceptionCheck(env))problem(env,"Native method registration failed");return result;
}

void klj_initialize_guest(JNIEnv *env,jlong image){if(arm32_image(image)){arm32_api->initialize(env,arm32_handle(image));return;}jint(*onload)(JavaVM*,void*)=find_symbol((kl_image*)(uintptr_t)image,"JNI_OnLoad");if(!onload)return;JavaVM *vm=NULL;if((*env)->GetJavaVM(env,&vm)!=JNI_OK)return;jint version=0;KLJ_INVOKE_GUEST(env,version=onload(klj_guest_vm(vm),NULL));if((*env)->ExceptionCheck(env))return;if(version<0)problem(env,"Guest JNI_OnLoad failed");}
void klj_construct_guest(JNIEnv *env,jlong image){construct(env,(kl_image*)(uintptr_t)image);}
void klj_reject_guest(jlong image){if(image&&reject_initialization)reject_initialization((kl_image*)(uintptr_t)image);}
