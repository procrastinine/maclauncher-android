#pragma once
#include <jni.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Main thread owns the layer. Original Android composition and native
 * graphics producers submit their completed images to this device presenter. */
void *klj_metal_create(JNIEnv *env,void *layer,int width,int height);
void klj_metal_enter(void *presenter);
int klj_metal_resize(JNIEnv *env,int width,int height);
int klj_metal_output_size(JNIEnv*,void *presenter,int width,int height);
int klj_metal_current(JNIEnv *env,void *surface);
void *klj_metal_surface(JNIEnv *env,void *display,void *config,int width,int height,bool opaque,const int *attributes);
int klj_metal_present(JNIEnv *env,void *presenter,int frame);
/* The EGL window transfers its submitted backing-image lease. Only an
 * explicitly preserved swap copies pixels into the next producer image. */
typedef struct KLMetalBufferSwap {void *surface,*storage,*context;int(*advance)(void*);bool preserved;} KLMetalBufferSwap;
int klj_metal_present_retained(JNIEnv*,void *presenter,int frame,void *owner,void(*release)(void*),const KLMetalBufferSwap*);
int klj_metal_replace_surface(JNIEnv*,void *presenter,void *surface,void *storage,int width,int height);
/* C EGL failures use EGL error values and never create a Java exception.
 * A safely retired failed command can return 1 with a nonzero error. */
#define KL_METAL_C_ERROR_ABI 1
typedef enum KLMetalError {
 KL_METAL_OK=0,KL_METAL_BAD_ACCESS=0x3002,KL_METAL_BAD_ALLOC=0x3003,
 KL_METAL_BAD_MATCH=0x3009,KL_METAL_BAD_PARAMETER=0x300c,
 KL_METAL_BAD_SURFACE=0x300d,KL_METAL_CONTEXT_LOST=0x300e
} KLMetalError;
void *klj_metal_surface_c(void*,void*,int,int,bool,const int*,KLMetalError*);
int klj_metal_current_c(void*,KLMetalError*);
int klj_metal_resize_c(int,int,KLMetalError*);
int klj_metal_replace_surface_c(void*,void*,void*,int,int,KLMetalError*);
int klj_metal_present_c(void*,int,KLMetalError*);
int klj_metal_present_retained_c(void*,int,void*,void(*)(void*),const KLMetalBufferSwap*,KLMetalError*);
int klj_metal_finish_c(void*,KLMetalError*);
int klj_metal_detach_surface_c(KLMetalError*);
/* Same owner/common pipeline, a top-left projection already encoded in cb.
 * Returns 0 accepted, 3 busy, 4 failure. Caller commits exactly once. */
int klj_metal_project(JNIEnv*,void *presenter,void *display,void *cb,void *texture);
/* Transfers an actual immutable image/pool lease, including rejected calls. */
int klj_metal_project_retained(JNIEnv*,void *presenter,void *display,void *cb,void *texture,void *owner,void(*release)(void*));
void *klj_metal_projection_queue(JNIEnv*,void *presenter,void *device);
typedef int (*KLNativeMetalFrame)(void *context,void *commandBuffer,void *texture);
/* Native driver WSI owns this retained carrier; output uses this presenter. */
void *klj_metal_native_layer(JNIEnv*,int width,int height,KLNativeMetalFrame,void *context);
void klj_metal_release_native_layer(void *layer);
int klj_metal_native_frame(JNIEnv*,void *presenter,void *commandBuffer,void *texture);
/* 1 means safe retirement, even when a terminal GPU error remains pending in
 * env. 0 retains live dependencies; it is not a rendered-frame verdict. */
int klj_metal_finish(JNIEnv *env,void *presenter);
uint64_t klj_metal_copy(JNIEnv*,void *presenter,void *storage,int width,int height,jobject callback);
uint64_t klj_metal_copy_region(JNIEnv*,void *presenter,void *storage,int left,int top,int right,int bottom,int width,int height,jobject callback);
void klj_metal_destroy(void *presenter);
uint64_t klj_metal_committed_extent(void *presenter);
/* Surface target setter: returns zero or an EGL error. */
unsigned klj_metal_target(void *presenter,void *display,void *surface,int64_t nanos);
int klj_metal_target_timing(void *presenter,void *result);
#ifdef __cplusplus
}
#endif
