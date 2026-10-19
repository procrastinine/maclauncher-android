#include <jni.h>
// Explicit ownership prevents a JVM load from invoking a dependency initializer.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *reserved){(void)vm;(void)reserved;return JNI_VERSION_1_6;}
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kl_jni.h"
#include "kl_env.h"
#include "memory-info.h"

static char *owned_assets,*owned_files;
static void fail(JNIEnv *env,const char *name,const char *message) {
    jclass cls=(*env)->FindClass(env,name);
    if(cls)(*env)->ThrowNew(env,cls,message);
}

JNIEXPORT void JNICALL Java_maclauncher_android_bridge_Bridge_configureNative(JNIEnv *env, jclass c, jstring assets, jstring files) {
    (void)c;
    const char *a = (*env)->GetStringUTFChars(env, assets, NULL);
    if(!a)return;
    const char *f = (*env)->GetStringUTFChars(env, files, NULL);
    if(!f){(*env)->ReleaseStringUTFChars(env,assets,a);return;}
    // Klepton retains absolute path strings, so the bridge owns these copies.
    char *next_assets=strdup(a),*next_files=strdup(f);
    if(next_assets&&next_files){kl_jni_set_assets_dir(next_assets);kl_jni_set_files_dir(next_files);free(owned_assets);free(owned_files);owned_assets=next_assets;owned_files=next_files;}
    else{free(next_assets);free(next_files);fail(env,"java/lang/OutOfMemoryError","Cannot configure Android storage paths");}
    (*env)->ReleaseStringUTFChars(env, assets, a);
    (*env)->ReleaseStringUTFChars(env, files, f);
}

// Cold service queries reuse the same native budget reported by Klepton's
// /proc adapter and memory-pressure monitor, with no duplicate Java estimator.
extern uint64_t kl_mem_budget_bytes(void);
extern uint64_t kl_mem_available_bytes(void);
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_Bridge_memoryInfo(JNIEnv *env,jclass type,jlongArray output) {
    (void)type;
    if(!output||(*env)->GetArrayLength(env,output)!=4){fail(env,"java/lang/IllegalArgumentException","Memory info requires four values");return;}
    jlong values[4];klj_memory_info_values(kl_mem_budget_bytes(),kl_mem_available_bytes(),kl_env_int("KL_LOWMEM_AT",80),values);
    (*env)->SetLongArrayRegion(env,output,0,4,values);
}

extern void klj_ndk_choreographer_shutdown(void);
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_shutdownDisplayCallbacks(JNIEnv *env,jclass type){(void)env;(void)type;klj_ndk_choreographer_shutdown();}
