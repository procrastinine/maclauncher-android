#include <jni.h>
#include "bridge.h"

JNIEXPORT jint JNICALL Java_maclauncher_android_ArtFilesystem_bindPackagePath(JNIEnv *env,jclass type,jbyteArray packet){
 (void)type;
 jsize size=(*env)->GetArrayLength(env,packet);
 jbyte *bytes=(*env)->GetByteArrayElements(env,packet,0);
 if(!bytes)return -12;
 int result=(int)klj_art_host(13,bytes,(size_t)size);
 (*env)->ReleaseByteArrayElements(env,packet,bytes,JNI_ABORT);
 return result;
}
