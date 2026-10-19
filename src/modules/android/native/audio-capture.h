#ifndef KLJ_AUDIO_CAPTURE_H
#define KLJ_AUDIO_CAPTURE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KljAudioCapture KljAudioCapture;
KljAudioCapture *klj_audio_capture_create(unsigned rate,unsigned channels,int format);
void klj_audio_capture_destroy(KljAudioCapture *);
int32_t klj_audio_capture_read(KljAudioCapture *,void *,int32_t,int64_t);
unsigned klj_audio_capture_rate(const KljAudioCapture *);
unsigned klj_audio_capture_channels(const KljAudioCapture *);
#ifdef __cplusplus
}
#endif
#endif
