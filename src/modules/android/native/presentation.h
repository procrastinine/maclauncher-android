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
/* Same owner/common pipeline, a top-left projection already encoded in cb.
 * Returns 0 accepted, 3 busy, 4 failure. Caller commits exactly once. */
int klj_metal_project(JNIEnv*,void *presenter,void *display,void *cb,void *texture);
void *klj_metal_projection_queue(JNIEnv*,void *presenter,void *device);
typedef int (*KLNativeMetalFrame)(void *context,void *commandBuffer,void *texture);
/* Native driver WSI owns this retained carrier; output uses this presenter. */
void *klj_metal_native_layer(JNIEnv*,int width,int height,KLNativeMetalFrame,void *context);
void klj_metal_release_native_layer(void *layer);
int klj_metal_native_frame(JNIEnv*,void *presenter,void *commandBuffer,void *texture);
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
