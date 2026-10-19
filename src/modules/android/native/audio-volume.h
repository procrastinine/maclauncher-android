#ifndef KLJ_AUDIO_VOLUME_H
#define KLJ_AUDIO_VOLUME_H
#include <stddef.h>
#define KLJ_AUDIO_STREAMS 12
#ifdef __cplusplus
extern "C" {
#endif
/* Session-local gain, never a hardware/system volume operation. */
int klj_audio_set_stream_volume(int stream,int index);
float klj_audio_stream_gain(int stream);
void klj_audio_scale_pcm(float *samples,size_t count,int stream);
/* Optional process-lifetime output filter. Runs on the existing render thread,
 * after mixing and stream gain. The callback must be realtime-safe;
 * its code remains loaded until process exit. NULL leaves output untouched. */
typedef void (*KljAudioOutputFilter)(float *samples,size_t count);
void klj_audio_set_output_filter(KljAudioOutputFilter filter);
void klj_audio_filter_output(float *samples,size_t count);
/* Native producer state, independent of muted gain or silent samples. */
void klj_audio_activity_transition(int stream,int was_playing,int playing);
int klj_audio_stream_active(int stream,int in_past_ms);
#ifdef __cplusplus
}
#endif
#endif
