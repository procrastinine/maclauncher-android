#ifndef KLJ_MEDIA_PLATFORM_H
#define KLJ_MEDIA_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* UTF-8 strings/size arrays returned here are caller-owned malloc storage.
 * Device selection and capture state are local to this application process. */
enum {KLJ_MEDIA_INVALID=-1,KLJ_MEDIA_CONFIGURED=-2,KLJ_MEDIA_NOT_FOUND=-3,KLJ_MEDIA_DENIED=-4};
char *klj_media_devices(void);
int klj_media_configure(const char *audioUID,const char *cameraUID);
int klj_media_authorized(int video);
uint32_t klj_media_audio_device(void);
void klj_media_shutdown(void);
int64_t klj_media_audio_open(int rate,int channels);
int64_t klj_media_audio_open_format(int rate,int channels,int format);
int klj_media_audio_start(uint64_t token);
int klj_media_audio_read(uint64_t token,int16_t *samples,size_t capacity);
int klj_media_audio_read_frames(uint64_t token,void *samples,size_t frames);
void klj_media_audio_stop(uint64_t token);
void klj_media_audio_close(uint64_t token);
int *klj_media_camera_sizes(size_t *count);
int64_t klj_media_camera_open(void);
int klj_media_camera_start(uint64_t token,int width,int height);
int klj_media_camera_read(uint64_t token,void *pixels,size_t capacity);
void klj_media_camera_stop(uint64_t token);
void klj_media_camera_close(uint64_t token);
#ifdef __cplusplus
}
#endif
#endif
