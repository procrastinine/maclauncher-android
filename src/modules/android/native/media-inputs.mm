#include <jni.h>
#import <Foundation/Foundation.h>
#include "media-platform.h"
#include <vector>
#include <string>
#include <cstdlib>
static void fail(JNIEnv *env,const char *type,const char *message){if(!env->ExceptionCheck()){jclass cls=env->FindClass(type);if(cls)env->ThrowNew(cls,message);}}
static std::string utf8(JNIEnv *env,jstring value){
    if(!value)return {};jsize count=env->GetStringLength(value);if(count>512){fail(env,"java/lang/IllegalArgumentException","Media identity too long");return {};}
    const jchar *chars=env->GetStringChars(value,nullptr);if(!chars)return {};
    NSString *text=[[NSString alloc]initWithCharacters:chars length:count];env->ReleaseStringChars(value,chars);return std::string(text.UTF8String);
}
static jstring string(JNIEnv *env,const char *value){
    if(!value)return nullptr;NSString *text=[NSString stringWithUTF8String:value];if(!text)return nullptr;
    std::vector<jchar> chars(text.length);[text getCharacters:chars.data() range:NSMakeRange(0,text.length)];return env->NewString(chars.data(),(jsize)chars.size());
}
extern "C" JNIEXPORT jstring JNICALL Java_maclauncher_android_media_MediaInputs_nativeDevices(JNIEnv *env,jclass){@autoreleasepool{
    char *json=klj_media_devices();if(!json){fail(env,"java/lang/OutOfMemoryError","Media device description is unavailable");return nullptr;}
    jstring result=string(env,json);free(json);return result;
}}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_nativeConfigure(JNIEnv *env,jclass,jstring audio,jstring video){@autoreleasepool{
    auto a=utf8(env,audio),v=utf8(env,video);if(env->ExceptionCheck())return;
    int result=klj_media_configure(a.c_str(),v.c_str());
    if(result==KLJ_MEDIA_INVALID)fail(env,"java/lang/IllegalArgumentException","Media identity is invalid");
    else if(result==KLJ_MEDIA_NOT_FOUND)fail(env,"java/lang/IllegalStateException","Selected audio input is unavailable");
    else if(result)fail(env,"java/lang/IllegalStateException","Media input owner is unavailable");
}}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_MediaInputs_nativeAuthorization(JNIEnv*,jclass,jboolean video){return klj_media_authorized(video);}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_nativeShutdown(JNIEnv*,jclass){klj_media_shutdown();}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_media_MediaInputs_audioOpen(JNIEnv *env,jclass,jint rate,jint channels){
    int64_t result=klj_media_audio_open(rate,channels);if(result==KLJ_MEDIA_DENIED){fail(env,"java/lang/SecurityException","Microphone permission is unavailable");return 0;}return result;
}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_MediaInputs_audioStart(JNIEnv*,jclass,jlong token){return klj_media_audio_start(token);}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_MediaInputs_audioRead(JNIEnv *env,jclass,jlong token,jshortArray data,jint offset,jint samples){
    if(!data||offset<0||samples<0||samples>8192||offset>env->GetArrayLength(data)-samples)return -2;
    int16_t buffer[8192];int count=klj_media_audio_read(token,buffer,(size_t)samples);if(count>0)env->SetShortArrayRegion(data,offset,count,buffer);return count;
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_audioStop(JNIEnv*,jclass,jlong token){klj_media_audio_stop(token);}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_audioClose(JNIEnv*,jclass,jlong token){klj_media_audio_close(token);}
extern "C" JNIEXPORT jintArray JNICALL Java_maclauncher_android_media_MediaInputs_cameraSizes(JNIEnv *env,jclass){
    size_t count=0;int *sizes=klj_media_camera_sizes(&count);if(count&&!sizes){fail(env,"java/lang/OutOfMemoryError","Camera sizes are unavailable");return nullptr;}
    jintArray result=env->NewIntArray((jsize)count);if(result&&count)env->SetIntArrayRegion(result,0,(jsize)count,sizes);free(sizes);return result;
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_media_MediaInputs_cameraOpen(JNIEnv *env,jclass){
    int64_t result=klj_media_camera_open();if(result==KLJ_MEDIA_DENIED){fail(env,"java/lang/SecurityException","Camera permission is unavailable");return 0;}return result;
}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_MediaInputs_cameraStart(JNIEnv*,jclass,jlong token,jint width,jint height){return klj_media_camera_start(token,width,height);}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_MediaInputs_cameraRead(JNIEnv *env,jclass,jlong token,jbyteArray bytes){
    if(!bytes)return klj_media_camera_read(token,nullptr,0);
    jbyte *output=env->GetByteArrayElements(bytes,nullptr);if(!output)return -1;
    int result=klj_media_camera_read(token,output,(size_t)env->GetArrayLength(bytes));env->ReleaseByteArrayElements(bytes,output,result>0?0:JNI_ABORT);return result;
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_cameraStop(JNIEnv*,jclass,jlong token){klj_media_camera_stop(token);}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_MediaInputs_cameraClose(JNIEnv*,jclass,jlong token){klj_media_camera_close(token);}
