#ifndef KLJ_AUDIO_HARDWARE_H
#define KLJ_AUDIO_HARDWARE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Read the live native PCM owner without opening a device. Output format uses
 * AAudio PCM_FLOAT (2); capture uses its native PCM_I16 (1). Returns AAUDIO_OK,
 * AAUDIO_ERROR_UNAVAILABLE or AAUDIO_ERROR_NULL; failure leaves outputs alone. */
int klj_audio_hardware_info(int32_t *rate,int32_t *channels,int32_t *format);
int klj_audio_capture_hardware_info(int32_t *rate,int32_t *channels,int32_t *format);
/* One stream lifetime, published at the existing mixer's committed writes.
 * Reading progress never takes a producer/device mutex or registers a source.
 * Retirement follows feeder join; destruction follows the final API use. */
typedef struct KljAudioSourceProgress KljAudioSourceProgress;
KljAudioSourceProgress *klj_audio_source_create(void);
size_t klj_audio_source_write(KljAudioSourceProgress *progress,const void *source,const void *pcm,size_t bytes,uint64_t frames);
/* maximum is the original stream's published written count. Blocking writes
 * can commit PCM before that counter is advanced by the original API body. */
uint64_t klj_audio_source_frames_read(KljAudioSourceProgress *progress,uint64_t maximum);
void klj_audio_source_retire(KljAudioSourceProgress *progress,const void *source);
void klj_audio_source_destroy(KljAudioSourceProgress *progress);
#ifdef __cplusplus
}
#endif
#endif
