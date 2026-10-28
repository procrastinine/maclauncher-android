#ifndef KLJ_AUDIO_HARDWARE_H
#define KLJ_AUDIO_HARDWARE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Read the live native PCM owner without opening a device. Output format uses
 * AAudio PCM_FLOAT (2); capture uses its native PCM_I16 (1). Returns AAUDIO_OK,
 * AAUDIO_ERROR_UNAVAILABLE or AAUDIO_ERROR_NULL; failure leaves outputs alone. */
int klj_audio_hardware_info(int32_t *rate,int32_t *channels,int32_t *format);
int klj_audio_capture_hardware_info(int32_t *rate,int32_t *channels,int32_t *format);
/* Pending PCM belonging to an existing producer, in its source-rate frames.
 * The query never creates a source or changes concurrent-source mixing. */
int klj_audio_source_queued_frames(const void *source,int32_t rate,uint64_t *frames);
#ifdef __cplusplus
}
#endif
#endif
