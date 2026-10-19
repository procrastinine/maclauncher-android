#include "guest/runtime-jni.h"
#include "../native-import-registry.h"
#include "../codec2/audio/Device.h"
#include <android_runtime/AndroidRuntime.h>
#include <dlfcn.h>
#include <cstring>
#include <string>
#include <cerrno>

namespace {
struct Facades {
    JNIEnv *(*environment)(JNIEnv *);
    JavaVM *(*vm)(JavaVM *);
};

/* The JVM already owns libnativebinding for its entire lifetime. Resolve its
 * existing representation adapters from that exact neighboring image; JNI
 * libraries need not be visible in the process's global dlsym namespace. */
Facades facadeOwner() {
    Dl_info image{};
    if(!dladdr(reinterpret_cast<void *>(&klj_runtime_environment),&image)||!image.dli_fname)
        return {};
    std::string file(image.dli_fname);
    auto slash=file.find_last_of('/');
    if(slash==std::string::npos)return {};
    file.resize(slash+1);file+="libnativebinding.dylib";
    void *owner=dlopen(file.c_str(),RTLD_LAZY|RTLD_NOLOAD);
    if(!owner)return {};
    Facades result{
        reinterpret_cast<JNIEnv *(*)(JNIEnv *)>(dlsym(owner,"klj_guest_env")),
        reinterpret_cast<JavaVM *(*)(JavaVM *)>(dlsym(owner,"klj_guest_vm"))
    };
    dlclose(owner);
    return result;
}

const Facades& facades() {
    static const Facades value=facadeOwner();
    return value;
}

void unavailable(JNIEnv *env) {
    if(env->ExceptionCheck())return;
    jclass type=env->FindClass("java/lang/UnsatisfiedLinkError");
    if(type){env->ThrowNew(type,"Android JVM representation owner is unavailable");env->DeleteLocalRef(type);}
}

void *resolveRuntime(const char *name) {
    if(!std::strcmp(name,"klj_runtime_environment"))return reinterpret_cast<void *>(&klj_runtime_environment);
    if(!std::strcmp(name,"klj_runtime_java_vm"))return reinterpret_cast<void *>(&klj_runtime_java_vm);
    if(!std::strcmp(name,"klj_runtime_main_queue_wake"))return reinterpret_cast<void *>(&klj_runtime_main_queue_wake);
    if(!std::strcmp(name,"klj_audio_input_selection"))return reinterpret_cast<void *>(&klj_audio_input_selection);
    return nullptr;
}
}

void klj_runtime_main_queue_wake() {
    using Wake=void(*)();
    static auto wake=reinterpret_cast<Wake>(dlsym(RTLD_DEFAULT,"kl_host_scheduler_notify"));
    if(wake)wake();
}

extern "C" int klj_audio_input_selection(uint32_t uid,uint32_t pid,char* address,size_t capacity){
    if(!address||!capacity)return -EINVAL;*address=0;
    JNIEnv* env=android::AndroidRuntime::getJNIEnv();
    if(!env||env->ExceptionCheck())return -EIO;
    // This is an integer-status device callback on an original Binder worker.
    // Convert only exceptions raised by this lookup, so that worker does not
    // retain a pending Java exception across unrelated native transactions.
    auto failed=[&]{if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();}return -EACCES;};
    jclass owner=env->FindClass("maclauncher/android/framework/HostPermissions");if(!owner)return failed();
    jmethodID method=env->GetStaticMethodID(owner,"audioInputForProcess","(II)Ljava/lang/String;");
    auto selected=method?static_cast<jstring>(env->CallStaticObjectMethod(owner,method,jint(uid),jint(pid))):nullptr;
    env->DeleteLocalRef(owner);if(env->ExceptionCheck()){if(selected)env->DeleteLocalRef(selected);return failed();}if(!selected)return -EACCES;
    const char* value=env->GetStringUTFChars(selected,nullptr);int result=-EACCES;
    using Resolve=int(*)(const char*,char*,size_t);
    if(value&&*value)if(auto resolve=reinterpret_cast<Resolve>(klj_resolve_import("klj_audio_device_address")))result=resolve(value,address,capacity);
    if(value)env->ReleaseStringUTFChars(selected,value);env->DeleteLocalRef(selected);return env->ExceptionCheck()?failed():result;
}

JNIEnv *klj_runtime_environment() {
    JNIEnv *env=android::AndroidRuntime::getJNIEnv();
    const auto &owner=facades();
    if(!owner.environment){unavailable(env);return nullptr;}
    return owner.environment(env);
}

JavaVM *klj_runtime_java_vm() {
    JNIEnv *env=android::AndroidRuntime::getJNIEnv();
    const auto &owner=facades();
    if(!owner.vm){unavailable(env);return nullptr;}
    JavaVM *vm=nullptr;
    return env->GetJavaVM(&vm)==JNI_OK?owner.vm(vm):nullptr;
}

bool kljInstallRuntimeImports() {
    return klj_register_import_resolver(resolveRuntime)!=0;
}
