#pragma once
#include <jni.h>
#include "bridge.h"
#include <dlfcn.h>
#include <atomic>
#include <binder/IBinder.h>
#include <nativehelper/JNIHelp.h>
#include "core_jni_helpers.h"

// Only the Java/native object conversion belongs to the VM. Parcel, RPC,
// identity and lifetime remain the complete shared Binder implementation.
namespace {
std::atomic<JavaVM*> artVm;
std::atomic<uid_t> artHostUid{static_cast<uid_t>(-1)};
JNIEnv* artJniEnvironment(){
    auto* vm=artVm.load(std::memory_order_acquire);
    return vm?android::GetOrAttachJNIEnvironment(vm):nullptr;
}
template<class Function> Function artJniSymbol(JNIEnv* env,const char* name){
    auto function=reinterpret_cast<Function>(dlsym(RTLD_DEFAULT,name));
    if(!function)jniThrowException(env,"java/lang/UnsatisfiedLinkError",name);
    return function;
}
android::sp<android::IBinder> artBinderForJavaObject(JNIEnv* env,jobject value){
    using Function=android::sp<android::IBinder>(*)(JNIEnv*,jobject);
    auto function=artJniSymbol<Function>(env,"_ZN7android20ibinderForJavaObjectEP7_JNIEnvP8_jobject");
    return function?function(env,value):nullptr;
}
jobject artJavaObjectForBinder(JNIEnv* env,const android::sp<android::IBinder>& value){
    using Function=jobject(*)(JNIEnv*,const android::sp<android::IBinder>&);
    auto function=artJniSymbol<Function>(env,"_ZN7android20javaObjectForIBinderEP7_JNIEnvRKNS_2spINS_7IBinderEEE");
    return function?function(env,value):nullptr;
}
}
extern "C" __attribute__((visibility("default"))) void klj_art_rpc_initialize(JavaVM* vm,uid_t hostUid){
    artHostUid.store(hostUid,std::memory_order_relaxed);
    artVm.store(vm,std::memory_order_release);
}
// Original GetOrAttachJNIEnvironment owns one attachment and its thread-local
// detacher. Native-only services have no VM; already attached callers keep
// their current owner. No Binder objects or interface methods are replaced.
extern "C" void klj_art_rpc_bind_native_thread(){(void)artJniEnvironment();}
