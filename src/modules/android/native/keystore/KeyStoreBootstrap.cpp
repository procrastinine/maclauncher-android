#include <jni.h>
// Explicit ownership prevents a JVM load from invoking a dependency initializer.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *reserved){return JNI_VERSION_1_6;}
#include <android_util_Binder.h>
#include <binder/IServiceManager.h>
#include <binder/IServiceManagerUnitTestHelper.h>
#include <android/os/IServiceManager.h>
#include <android/binder_manager.h>
#include <AndroidKeyMintDevice.h>
#include <AndroidRemotelyProvisionedComponentDevice.h>
#include <AndroidSecureClock.h>
#include <AndroidSharedSecret.h>
#include <mutex>
#include <stdexcept>

extern "C" int kl_keystore_start(const char*);
extern "C" int kl_keystore_bind_software_keymint(AIBinder*);

namespace {
std::once_flag startup;
template<class T> void publish(const std::shared_ptr<T>& service,const char* suffix) {
    const std::string name=std::string(T::descriptor)+suffix;
    if(AServiceManager_addService(service->asBinder().get(),name.c_str())!=STATUS_OK)
        throw std::runtime_error("Unable to publish Android service: "+name);
}
void initialize(JNIEnv* env,jobject manager,const char* directory) {
    auto endpoint=android::ibinderForJavaObject(env,manager);
    if(!endpoint)throw std::runtime_error("Android service directory is missing");
    auto service=android::interface_cast<android::os::IServiceManager>(endpoint);
    android::setDefaultServiceManager(android::getServiceManagerShimFromAidlServiceManagerForTests(service));
    namespace km=aidl::android::hardware::security::keymint;
    auto keymint=ndk::SharedRefBase::make<km::AndroidKeyMintDevice>(km::SecurityLevel::SOFTWARE);
    if(kl_keystore_bind_software_keymint(keymint->asBinder().get())!=0)
        throw std::runtime_error("Unable to initialize the software KeyMint owner");
    publish(keymint,"/default");
    publish(ndk::SharedRefBase::make<km::AndroidRemotelyProvisionedComponentDevice>(keymint),"/default");
    publish(ndk::SharedRefBase::make<aidl::android::hardware::security::secureclock::AndroidSecureClock>(keymint),"/default");
    publish(ndk::SharedRefBase::make<aidl::android::hardware::security::sharedsecret::AndroidSharedSecret>(keymint),"/default");
    if(kl_keystore_start(directory)!=0)throw std::runtime_error("Android KeyStore service initialization failed");
}
}

extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostKeyStore_startNative(
        JNIEnv* env,jclass,jobject manager,jstring database) {
    if(!database){jclass type=env->FindClass("java/lang/NullPointerException");if(type)env->ThrowNew(type,"KeyStore database directory is missing");return;}
    const char* directory=env->GetStringUTFChars(database,nullptr);if(!directory)return;
    try{std::call_once(startup,[&]{initialize(env,manager,directory);});}
    catch(const std::exception& failure){if(!env->ExceptionCheck()){jclass type=env->FindClass("java/lang/IllegalStateException");if(type)env->ThrowNew(type,failure.what());}}
    env->ReleaseStringUTFChars(database,directory);
}
