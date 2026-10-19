#include <jni.h>
#include "web-links-platform.h"

// VM references stop here; the same Mac channel owner implements both VMs.
JNIEXPORT jboolean JNICALL Java_maclauncher_android_bridge_WebLinks_configureNative(JNIEnv*env,jclass type,jboolean allowed){
    (void)type;int result=klj_web_links_configure(allowed);
    if(result<0){jclass error=(*env)->FindClass(env,"java/lang/IllegalStateException");if(error){(*env)->ThrowNew(env,error,"Web-link policy is already configured");(*env)->DeleteLocalRef(env,error);}}
    return result>0;
}
JNIEXPORT jboolean JNICALL Java_maclauncher_android_bridge_WebLinks_openNative(JNIEnv*env,jclass type,jbyteArray url){
    (void)type;if(!url)return JNI_FALSE;jsize length=(*env)->GetArrayLength(env,url);if(length<=0||length>8192)return JNI_FALSE;
    unsigned char bytes[8192];(*env)->GetByteArrayRegion(env,url,0,length,(jbyte*)bytes);return !(*env)->ExceptionCheck(env)&&klj_web_links_open(bytes,(size_t)length);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_WebLinks_closeNative(JNIEnv*env,jclass type){(void)env;(void)type;klj_web_links_close();}
