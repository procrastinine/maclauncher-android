#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <CoreVideo/CoreVideo.h>
#include <media/NdkMediaFormat.h>
#include "media-surface.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct KLJCodec KLJCodec;
typedef struct {
    int32_t offset,size;
    int64_t presentationTimeUs;
    uint32_t flags;
} KLJCodecBufferInfo;

typedef KLJMediaSurface KLJCodecSurface;

typedef struct {
    void *context;
    void (*retain)(void*);
    void (*release)(void*);
    void (*input)(void*,size_t);
    void (*output)(void*,size_t,const KLJCodecBufferInfo*);
    void (*format)(void*,const AMediaFormat*);
    void (*error)(void*,int);
} KLJCodecCallbacks;

typedef struct {
    void *context;
    void (*retain)(void*);
    void (*release)(void*);
    void (*rendered)(void*,int64_t mediaTimeUs,int64_t systemNano);
} KLJCodecRenderedCallbacks;

KLJCodec *klj_codec_create(const char *name,bool nameIsType,bool encoder);
void klj_codec_retain(KLJCodec*);
void klj_codec_release(KLJCodec*);
int klj_codec_close(KLJCodec*);
int klj_codec_reset(KLJCodec*);
int klj_codec_configure(KLJCodec*,const AMediaFormat*,const KLJCodecSurface*,unsigned flags);
int klj_codec_set_surface(KLJCodec*,const KLJCodecSurface*);
int klj_codec_start(KLJCodec*);
int klj_codec_stop(KLJCodec*);
int klj_codec_flush(KLJCodec*);
ssize_t klj_codec_dequeue_input(KLJCodec*,int64_t timeoutUs);
uint8_t *klj_codec_input_buffer(KLJCodec*,size_t,size_t*);
uint8_t *klj_codec_array_buffer(KLJCodec*,bool input,size_t,size_t*);
int klj_codec_queue_input(KLJCodec*,size_t,off_t,size_t,int64_t,unsigned);
int klj_codec_queue_inputs(KLJCodec*,size_t,const KLJCodecBufferInfo*,size_t);
ssize_t klj_codec_dequeue_output(KLJCodec*,KLJCodecBufferInfo*,int64_t timeoutUs);
uint8_t *klj_codec_output_buffer(KLJCodec*,size_t,size_t*);
int klj_codec_release_output(KLJCodec*,size_t,bool render,int64_t renderTimeNs);
AMediaFormat *klj_codec_format(KLJCodec*,bool input);
AMediaFormat *klj_codec_buffer_format(KLJCodec*,size_t);
const char *klj_codec_name(KLJCodec*);
size_t klj_codec_input_count(KLJCodec*);
size_t klj_codec_output_count(KLJCodec*);
int klj_codec_callbacks(KLJCodec*,const KLJCodecCallbacks*);
int klj_codec_rendered_callbacks(KLJCodec*,const KLJCodecRenderedCallbacks*);
int klj_codec_parameters(KLJCodec*,const AMediaFormat*);
#ifdef __cplusplus
}
#endif
