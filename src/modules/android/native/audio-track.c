#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include "audio-pcm.h"
#include "audio-shaper.h"
typedef struct {void *source,*shaper;float *samples;size_t capacity;unsigned channels;} pcm_owner;
JNIEXPORT jlong JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkOpen(JNIEnv *env,jclass type,jint channels,jint rate){
    (void)env;(void)type;if(channels<1||rate<1)return 0;pcm_owner *owner=calloc(1,sizeof(*owner));if(!owner)return 0;
    // Two render bursts bound transport latency; the requested application
    // buffer lives in the AudioTrack owner and has no imposed admission cap.
    owner->channels=(unsigned)channels;owner->shaper=klj_audio_shaper_create((unsigned)rate);if(!owner->shaper){free(owner);return 0;}
    owner->source=klj_audio_pcm_create_shaped(((size_t)rate+99)/100,owner->channels,rate,klj_audio_shaper_gain,owner->shaper);if(!owner->source){klj_audio_shaper_destroy(owner->shaper);free(owner);return 0;}return (jlong)(uintptr_t)owner;
}
JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkWrite(JNIEnv *env,jclass type,jlong handle,jfloatArray pcm,jint offset,jint frames){
    (void)type;if(!handle||!pcm||offset<0||frames<0)return -2;
    pcm_owner *owner=(void*)(uintptr_t)handle;
    if(offset>(*env)->GetArrayLength(env,pcm)/(jint)owner->channels-frames)return -2;
    if((size_t)frames>owner->capacity){float *next=realloc(owner->samples,(size_t)frames*owner->channels*sizeof(float));if(!next)return -1;owner->samples=next;owner->capacity=(size_t)frames;}
    (*env)->GetFloatArrayRegion(env,pcm,offset*(jint)owner->channels,frames*(jint)owner->channels,owner->samples);if((*env)->ExceptionCheck(env))return -1;
    return klj_audio_pcm_write(owner->source,owner->samples,frames);
}
JNIEXPORT void JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkControls(JNIEnv *env,jclass type,jlong handle,jdouble rate,jfloat left,jfloat right,jint dual){(void)env;(void)type;pcm_owner *owner=(void*)(uintptr_t)handle;if(owner)klj_audio_pcm_controls(owner->source,rate,left,right,dual);}
JNIEXPORT void JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkState(JNIEnv *env,jclass type,jlong handle,jint state){(void)env;(void)type;pcm_owner *owner=(void*)(uintptr_t)handle;if(owner)klj_audio_pcm_state(owner->source,state);}
JNIEXPORT jlong JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkPosition(JNIEnv *env,jclass type,jlong handle){(void)env;(void)type;pcm_owner *owner=(void*)(uintptr_t)handle;return owner?(jlong)klj_audio_pcm_position(owner->source):0;}
JNIEXPORT jlong JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkQueued(JNIEnv *env,jclass type,jlong handle){(void)env;(void)type;pcm_owner *owner=(void*)(uintptr_t)handle;return owner?(jlong)klj_audio_pcm_queued(owner->source):0;}
JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkUnderruns(JNIEnv *env,jclass type,jlong handle){(void)env;(void)type;pcm_owner *owner=(void*)(uintptr_t)handle;return owner?(jint)klj_audio_pcm_underruns(owner->source):0;}
JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkApplyShaper(JNIEnv *env,jclass type,jlong handle,jobject config,jobject operation){(void)type;pcm_owner *owner=(void*)(uintptr_t)handle;return klj_audio_shaper_apply(env,owner?owner->shaper:NULL,config,operation);}
JNIEXPORT jobject JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkShaperState(JNIEnv *env,jclass type,jlong handle,jint id){(void)type;pcm_owner *owner=(void*)(uintptr_t)handle;return klj_audio_shaper_state(env,owner?owner->shaper:NULL,id);}
JNIEXPORT void JNICALL Java_maclauncher_android_media_HostAudioTrack_sinkClose(JNIEnv *env,jclass type,jlong handle){(void)env;(void)type;pcm_owner *owner=(void*)(uintptr_t)handle;if(owner){klj_audio_pcm_destroy(owner->source);klj_audio_shaper_destroy(owner->shaper);free(owner->samples);free(owner);}}
