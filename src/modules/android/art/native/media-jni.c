#include <jni.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
#include "media-bridge.h"
#include "media-platform.h"

static void fail(JNIEnv *env,const char *name,const char *message){
    if((*env)->ExceptionCheck(env))return;
    jclass type=(*env)->FindClass(env,name);if(type){(*env)->ThrowNew(env,type,message);(*env)->DeleteLocalRef(env,type);}
}
static int request(JNIEnv *env,struct KljArtMedia *value){
    int64_t result=klj_art_host(8,value,sizeof(*value));if(!result)return 1;
    fail(env,result==-12?"java/lang/OutOfMemoryError":"java/lang/IllegalStateException","Physical media operation failed");return 0;
}
/* JNI's modified UTF-8 is not the device identity encoding. Keep Java's exact
 * UTF-8 conversion in the VM and only transport the resulting native bytes. */
static char *utf8(JNIEnv *env,jstring value){
    if(!value){char *empty=calloc(1,1);if(!empty)fail(env,"java/lang/OutOfMemoryError","Media identity is unavailable");return empty;}
    if((*env)->GetStringLength(env,value)>512){fail(env,"java/lang/IllegalArgumentException","Media identity too long");return NULL;}
    jclass type=(*env)->FindClass(env,"java/lang/String");if(!type)return NULL;
    jmethodID method=(*env)->GetMethodID(env,type,"getBytes","(Ljava/lang/String;)[B");
    jstring encoding=method?(*env)->NewStringUTF(env,"UTF-8"):NULL;
    jbyteArray bytes=encoding?(*env)->CallObjectMethod(env,value,method,encoding):NULL;
    if(encoding)(*env)->DeleteLocalRef(env,encoding);(*env)->DeleteLocalRef(env,type);
    if((*env)->ExceptionCheck(env)||!bytes)return NULL;
    jsize length=(*env)->GetArrayLength(env,bytes);char *result=malloc((size_t)length+1);
    if(result){(*env)->GetByteArrayRegion(env,bytes,0,length,(jbyte*)result);result[length]=0;}
    else fail(env,"java/lang/OutOfMemoryError","Media identity is unavailable");
    (*env)->DeleteLocalRef(env,bytes);return result;
}
static void *fetch(JNIEnv *env,uint32_t operation,jsize *length){
    void *bytes=NULL;struct KljArtMedia value={.operation=operation};
    for(;;){
        if(!request(env,&value)){free(bytes);return NULL;}
        if(!value.result){*length=(jsize)value.length;return bytes;}
        if(value.length>INT_MAX){free(bytes);fail(env,"java/lang/OutOfMemoryError","Media data exceeds Java array size");return NULL;}
        void *next=realloc(bytes,(size_t)value.length);if(!next){free(bytes);fail(env,"java/lang/OutOfMemoryError","Media data is unavailable");return NULL;}
        bytes=next;value.data=(uintptr_t)bytes;value.capacity=value.length;
    }
}
JNIEXPORT jstring JNICALL Java_maclauncher_android_media_MediaInputs_nativeDevices(JNIEnv *env,jclass ignored){
    (void)ignored;jsize length=0;void *text=fetch(env,KLJ_ART_MEDIA_DEVICES,&length);if((*env)->ExceptionCheck(env))return NULL;
    jbyteArray bytes=(*env)->NewByteArray(env,length);if(bytes&&length)(*env)->SetByteArrayRegion(env,bytes,0,length,text);free(text);
    if(!bytes||(*env)->ExceptionCheck(env))return NULL;
    jclass type=(*env)->FindClass(env,"java/lang/String");jmethodID method=type?(*env)->GetMethodID(env,type,"<init>","([BLjava/lang/String;)V"):NULL;
    jstring encoding=method?(*env)->NewStringUTF(env,"UTF-8"):NULL;
    jstring result=encoding?(*env)->NewObject(env,type,method,bytes,encoding):NULL;
    if(encoding)(*env)->DeleteLocalRef(env,encoding);if(type)(*env)->DeleteLocalRef(env,type);(*env)->DeleteLocalRef(env,bytes);return result;
}
JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_nativeConfigure(JNIEnv *env,jclass ignored,jstring audio,jstring camera){
    (void)ignored;char *a=utf8(env,audio),*v=(*env)->ExceptionCheck(env)?NULL:utf8(env,camera);
    if((*env)->ExceptionCheck(env)||!a||!v){free(a);free(v);return;}
    struct KljArtMedia value={.operation=KLJ_ART_MEDIA_CONFIGURE,.data=(uintptr_t)a,.other=(uintptr_t)v};int ok=request(env,&value);free(a);free(v);if(!ok)return;
    if(value.result==KLJ_MEDIA_INVALID)fail(env,"java/lang/IllegalArgumentException","Media identity is invalid");
    else if(value.result==KLJ_MEDIA_NOT_FOUND)fail(env,"java/lang/IllegalStateException","Selected audio input is unavailable");
    else if(value.result)fail(env,"java/lang/IllegalStateException","Media input owner is unavailable");
}
JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_MediaInputs_nativeAuthorization(JNIEnv *env,jclass ignored,jboolean video){(void)ignored;struct KljArtMedia r={.operation=KLJ_ART_MEDIA_AUTHORIZED,.first=video};return request(env,&r)&&r.result;}
JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_nativeShutdown(JNIEnv *env,jclass ignored){(void)ignored;struct KljArtMedia r={.operation=KLJ_ART_MEDIA_SHUTDOWN};request(env,&r);}
JNIEXPORT jlong JNICALL Java_maclauncher_android_media_MediaInputs_audioOpen(JNIEnv *env,jclass ignored,jint rate,jint channels){
    (void)ignored;struct KljArtMedia r={.operation=KLJ_ART_MEDIA_AUDIO_OPEN,.first=rate,.second=channels};if(!request(env,&r))return 0;
    if(r.result==KLJ_MEDIA_DENIED){fail(env,"java/lang/SecurityException","Microphone permission is unavailable");return 0;}return r.result;
}
JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_MediaInputs_audioStart(JNIEnv *env,jclass ignored,jlong token){(void)ignored;struct KljArtMedia r={.operation=KLJ_ART_MEDIA_AUDIO_START,.token=token};return request(env,&r)&&r.result;}
JNIEXPORT jint JNICALL Java_maclauncher_android_media_MediaInputs_audioRead(JNIEnv *env,jclass ignored,jlong token,jshortArray data,jint offset,jint samples){
    (void)ignored;if(!data||offset<0||samples<0||samples>8192||offset>(*env)->GetArrayLength(env,data)-samples)return -2;
    int16_t buffer[8192];struct KljArtMedia r={.operation=KLJ_ART_MEDIA_AUDIO_READ,.token=token,.data=(uintptr_t)buffer,.capacity=(uint64_t)samples};if(!request(env,&r))return -6;
    if(r.result>0)(*env)->SetShortArrayRegion(env,data,offset,(jsize)r.result,buffer);return (jint)r.result;
}
static void control(JNIEnv *env,uint32_t operation,jlong token){struct KljArtMedia r={.operation=operation,.token=token};request(env,&r);}
JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_audioStop(JNIEnv *env,jclass ignored,jlong token){(void)ignored;control(env,KLJ_ART_MEDIA_AUDIO_STOP,token);}
JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_audioClose(JNIEnv *env,jclass ignored,jlong token){(void)ignored;control(env,KLJ_ART_MEDIA_AUDIO_CLOSE,token);}
JNIEXPORT jintArray JNICALL Java_maclauncher_android_media_MediaInputs_cameraSizes(JNIEnv *env,jclass ignored){
    (void)ignored;jsize bytes=0;int *values=fetch(env,KLJ_ART_MEDIA_CAMERA_SIZES,&bytes);if((*env)->ExceptionCheck(env))return NULL;
    jsize count=bytes/(jsize)sizeof(int);jintArray result=(*env)->NewIntArray(env,count);if(result&&count)(*env)->SetIntArrayRegion(env,result,0,count,values);free(values);return result;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_media_MediaInputs_cameraOpen(JNIEnv *env,jclass ignored){
    (void)ignored;struct KljArtMedia r={.operation=KLJ_ART_MEDIA_CAMERA_OPEN};if(!request(env,&r))return 0;
    if(r.result==KLJ_MEDIA_DENIED){fail(env,"java/lang/SecurityException","Camera permission is unavailable");return 0;}return r.result;
}
JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_MediaInputs_cameraStart(JNIEnv *env,jclass ignored,jlong token,jint width,jint height){(void)ignored;struct KljArtMedia r={.operation=KLJ_ART_MEDIA_CAMERA_START,.token=token,.first=width,.second=height};return request(env,&r)&&r.result;}
JNIEXPORT jint JNICALL Java_maclauncher_android_media_MediaInputs_cameraRead(JNIEnv *env,jclass ignored,jlong token,jbyteArray data){
    (void)ignored;jbyte *bytes=data?(*env)->GetByteArrayElements(env,data,NULL):NULL;if(data&&!bytes)return -1;
    struct KljArtMedia r={.operation=KLJ_ART_MEDIA_CAMERA_READ,.token=token,.data=(uintptr_t)bytes,.capacity=data?(uint64_t)(*env)->GetArrayLength(env,data):0};int ok=request(env,&r);
    if(bytes)(*env)->ReleaseByteArrayElements(env,data,bytes,ok&&r.result>0?0:JNI_ABORT);return ok?(jint)r.result:-1;
}
JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_cameraStop(JNIEnv *env,jclass ignored,jlong token){(void)ignored;control(env,KLJ_ART_MEDIA_CAMERA_STOP,token);}
JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_cameraClose(JNIEnv *env,jclass ignored,jlong token){(void)ignored;control(env,KLJ_ART_MEDIA_CAMERA_CLOSE,token);}
