#ifndef KLJ_AUDIO_SHAPER_H
#define KLJ_AUDIO_SHAPER_H
#include <jni.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *klj_audio_shaper_create(unsigned sample_rate);
void klj_audio_shaper_destroy(void *);
float klj_audio_shaper_gain(void *,uint64_t frame);
int klj_audio_shaper_apply(JNIEnv *,void *,jobject configuration,jobject operation);
jobject klj_audio_shaper_state(JNIEnv *,void *,int id);
#ifdef __cplusplus
}
#endif
#endif
