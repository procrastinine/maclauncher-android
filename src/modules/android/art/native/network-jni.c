#include <jni.h>
#include <errno.h>
#include <string.h>
#include "bridge.h"
static int request(JNIEnv*env,struct KljArtNetwork*value){
 int64_t result=klj_art_host(6,value,sizeof(*value));if(!result)return 1;
 if(result==-ECANCELED)return 0;
 jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");if(type)(*env)->ThrowNew(env,type,"Physical network observation failed");return 0;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_ArtNetwork_observe(JNIEnv*env,jclass type,jint operation,jlong epoch,jlong snapshot){
 (void)type;struct KljArtNetwork value={.operation=(uint32_t)operation,.token=(uint64_t)epoch,.snapshot=(uint64_t)snapshot};
 return request(env,&value)?(jlong)value.snapshot:-1;
}
JNIEXPORT void JNICALL Java_maclauncher_android_net_HostNetworkPolicy_configureNative(JNIEnv*env,jclass type,jboolean allowed){
 (void)type;struct KljArtNetwork value={.operation=5,.allowed=allowed};request(env,&value);
}
JNIEXPORT jboolean JNICALL Java_maclauncher_android_net_HostNetworkPolicy_hostAllowedNative(JNIEnv*env,jclass type,jstring host){
 (void)type;if(!host)return JNI_TRUE;const char*bytes=(*env)->GetStringUTFChars(env,host,NULL);if(!bytes)return JNI_FALSE;
 struct KljArtNetwork value={.operation=7,.buffer=(uintptr_t)bytes,.capacity=strlen(bytes)+1};
 int ok=request(env,&value);(*env)->ReleaseStringUTFChars(env,host,bytes);return ok&&value.allowed;
}
