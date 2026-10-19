#ifndef KLJ_AUDIO_PCM_H
#define KLJ_AUDIO_PCM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *klj_audio_pcm_create(size_t capacity,unsigned channels,double rate);
typedef float (*klj_audio_pcm_gain)(void *,uint64_t frame);
void *klj_audio_pcm_create_shaped(size_t capacity,unsigned channels,double rate,klj_audio_pcm_gain gain,void *owner);
/* Immutable PCM16 clips borrow their samples until destroy. Copies can share
 * those samples while retaining independent playback clocks and controls. */
void *klj_audio_pcm_clip(const int16_t *samples,size_t frames,unsigned channels,double rate);
void klj_audio_pcm_clip_loops(void *,int loops);
void klj_audio_pcm_clip_seek(void *,uint64_t frame);
int klj_audio_pcm_playing(void *);
void klj_audio_pcm_destroy(void *);
int klj_audio_pcm_write(void *,const float *,int);
void klj_audio_pcm_controls(void *,double,float,float,int);
void klj_audio_pcm_state(void *,int);
uint64_t klj_audio_pcm_position(void *);
uint64_t klj_audio_pcm_queued(void *);
unsigned klj_audio_pcm_underruns(void *);
void klj_audio_pcm_mix(float *,size_t,unsigned,double);
int klj_audio_pcm_device(void);
int klj_audio_pcm_has_sources(void);
#ifdef __cplusplus
}
#endif
#endif
