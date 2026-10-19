#include <jni.h>
#include <sys/types.h>
#include <android/log.h>
extern int kljRegisterBinderRpc(JNIEnv*);
extern "C" void klj_art_rpc_initialize(JavaVM*,uid_t);
extern "C" void klj_art_log_initialize(void);
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtPlatform_initializeRpc(JNIEnv* env,jclass,jint hostUid){
    if(hostUid<0){jclass type=env->FindClass("java/lang/IllegalArgumentException");if(type)env->ThrowNew(type,"Invalid native process owner");return;}
    JavaVM* vm=nullptr;if(env->GetJavaVM(&vm)!=JNI_OK)return;
    // Restore the same physical logger after RuntimeInit, before app code can
    // redirect guest stdio. The log component retains its original destination.
    klj_art_log_initialize();
    klj_art_rpc_initialize(vm,static_cast<uid_t>(hostUid));
    (void)kljRegisterBinderRpc(env);
}
