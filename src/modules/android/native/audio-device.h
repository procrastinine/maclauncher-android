#ifndef KLJ_AUDIO_DEVICE_H
#define KLJ_AUDIO_DEVICE_H
#include <CoreFoundation/CoreFoundation.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t id,channels,bufferFrames,latencyFrames,safetyFrames,transport,jack,alive,type;
    double rate;
    CFStringRef name,uid;
} KljAudioDevice;
/* Returned descriptions own their two CF strings. Locale/VM references never
 * enter this physical device boundary. Missing optional properties stay zero. */
int klj_audio_device_list(uint32_t **devices,size_t *count);
int klj_audio_device_default_output(uint32_t *device);
int klj_audio_device_get(uint32_t device,int input,KljAudioDevice *description);
void klj_audio_device_release(KljAudioDevice *description);
typedef struct KljAudioDeviceMonitor KljAudioDeviceMonitor;
typedef void (*KljAudioDeviceChanged)(void *);
KljAudioDeviceMonitor *klj_audio_device_monitor(KljAudioDeviceChanged,void *);
int klj_audio_device_monitor_close(KljAudioDeviceMonitor *);
uint32_t klj_audio_device_generation(void);
#ifdef __cplusplus
}
#endif
#endif
