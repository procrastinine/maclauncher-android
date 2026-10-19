#include "hotspot-jni.h"
#include "core_jni_helpers.h"
#include <android_runtime/AndroidRuntime.h>
#include <android/file_descriptor_jni.h>
#include <binder/IPCThreadState.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <dlfcn.h>
#include <string>
#include "../jni-lifetime.h"

namespace {
JavaVM* javaVM;
jclass fileDescriptorClass;
jmethodID fileDescriptorConstructor;
jfieldID fileDescriptorField;
JNIEnv* (*environment)(JavaVM*);
void retireBinder(JNIEnv*,int);
KljJniVmObserver observer{retireBinder,nullptr};
}

int register_android_os_Binder(JNIEnv*);
int kljRegisterObjectParcel(JNIEnv*);
int kljRegisterBinderRpc(JNIEnv*);
void kljCloseBinderRpc();
void kljSealBinderRpc();
void kljReleaseBinderRpcMetadata(JNIEnv*);
void kljReleaseObjectParcelMetadata(JNIEnv*);
void kljReleaseBinderJniMetadata(JNIEnv*);
bool kljInstallTraceImports();
namespace android {
int register_android_os_Parcel(JNIEnv*);
int register_android_os_SharedMemory(JNIEnv*);
int register_android_util_MemoryIntArray(JNIEnv*);
int register_android_util_CharsetUtils(JNIEnv*);
int register_android_util_CharsetArray(JNIEnv*);
void kljReleaseParcelJniMetadata(JNIEnv*);
void kljRetireServiceManager();
void kljRetireUnifiedServiceManager();
void kljRetireBinderMemory();
JNIEnv* AndroidRuntime::getJNIEnv(){return kljBinderEnvironment(javaVM);}
}
JNIEnv* kljBinderEnvironment(JavaVM* vm){return environment?environment(vm):nullptr;}

namespace {
void retireBinder(JNIEnv* env,int phase){
    kljSealBinderRpc();
    if(phase==KLJ_JNI_SHUTDOWN){
        kljCloseBinderRpc();
        android::IPCThreadState::self()->setTheContextObject(nullptr);
        android::kljRetireBinderMemory();
        android::kljRetireServiceManager();
        android::kljRetireUnifiedServiceManager();
    }else{
        // Shutdown already drained callbacks while Java could run. VMDeath
        // never waits on a worker or a Java looper.
        kljReleaseBinderRpcMetadata(env);
        kljReleaseObjectParcelMetadata(env);
        kljReleaseBinderJniMetadata(env);
        android::kljReleaseParcelJniMetadata(env);
        if(fileDescriptorClass){env->DeleteGlobalRef(fileDescriptorClass);fileDescriptorClass=nullptr;}
    }
}
bool observeLifetime(JNIEnv* env){
    Dl_info image{};
    if(!dladdr(reinterpret_cast<void*>(&observeLifetime),&image)||!image.dli_fname)return false;
    std::string file(image.dli_fname);auto slash=file.find_last_of('/');if(slash==std::string::npos)return false;
    file.resize(slash+1);file+="libnativebinding.dylib";
    void* owner=dlopen(file.c_str(),RTLD_LAZY|RTLD_NOLOAD);if(!owner)return false;
    auto observe=reinterpret_cast<int(*)(JNIEnv*,KljJniVmObserver*)>(dlsym(owner,"klj_jni_vm_observe"));
    environment=reinterpret_cast<JNIEnv*(*)(JavaVM*)>(dlsym(owner,"klj_jni_vm_environment"));
    bool result=observe&&environment&&observe(env,&observer);dlclose(owner);return result;
}
}

extern "C" int __android_log_write(int,const char* tag,const char* message){return fprintf(stderr,"%s: %s\n",tag?tag:"",message?message:"");}
extern "C" void __android_log_assert(const char* condition,const char* tag,const char* format,...){
    fprintf(stderr,"%s: %s: ",tag?tag:"Binder",condition?condition:"assertion");
    if(format){va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);}
    fputc('\n',stderr);abort();
}

// The complete AOSP JNI helpers use these NDK functions. Only the actual JDK
// FileDescriptor layout differs; Parcel still owns duplication and retirement.
extern "C" jobject AFileDescriptor_create(JNIEnv* env){return env->NewObject(fileDescriptorClass,fileDescriptorConstructor);}
extern "C" int AFileDescriptor_getFd(JNIEnv* env,jobject descriptor){return env->GetIntField(descriptor,fileDescriptorField);}
extern "C" void AFileDescriptor_setFd(JNIEnv* env,jobject descriptor,int fd){env->SetIntField(descriptor,fileDescriptorField,fd);}

extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostProcessIdentity_initializeBinderNative(JNIEnv*,jclass){
    // Binder may have created the bootstrap thread state before the supervisor
    // principal was read. Original clear/restore logic uses the same owner from
    // this point onward, including on every subsequently attached RPC thread.
    android::IPCThreadState::self()->clearCallingIdentity();
}

static jint initializeBinder(JavaVM* vm){
    javaVM=vm;JNIEnv* env=nullptr;
    if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK)return JNI_ERR;
    if(!observeLifetime(env))return JNI_ERR;
    jclass descriptor=env->FindClass("java/io/FileDescriptor");if(!descriptor)return JNI_ERR;
    fileDescriptorClass=static_cast<jclass>(env->NewGlobalRef(descriptor));env->DeleteLocalRef(descriptor);
    if(!fileDescriptorClass)return JNI_ERR;
    fileDescriptorConstructor=env->GetMethodID(fileDescriptorClass,"<init>","()V");if(!fileDescriptorConstructor)return JNI_ERR;
    fileDescriptorField=env->GetFieldID(fileDescriptorClass,"fd","I");if(!fileDescriptorField)return JNI_ERR;
    if(register_android_os_Binder(env)<0||android::register_android_os_Parcel(env)<0||android::register_android_os_SharedMemory(env)<0||kljRegisterObjectParcel(env)<0||kljRegisterBinderRpc(env)<0||
        android::register_android_util_CharsetUtils(env)<0||android::register_android_util_CharsetArray(env)<0||android::register_android_util_MemoryIntArray(env)<0)return JNI_ERR;
    if(!kljInstallTraceImports())return JNI_ERR;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm,void*){
    // JNI registration and retained class references belong to this library,
    // even when another native consumer also requests its initialization.
    static std::once_flag initialized;
    static jint version=JNI_ERR;
    std::call_once(initialized,[vm]{version=initializeBinder(vm);});
    return version;
}
