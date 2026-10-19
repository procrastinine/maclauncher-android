#pragma once
#include <CoreVideo/CoreVideo.h>
#include <jni.h>
#ifdef __cplusplus
extern "C" {
#endif
/* A retained existing Surface consumer, shared by Java and NDK media clients. */
typedef struct {
    void *context;
    void (*retain)(void*);
    void (*release)(void*);
    int (*present)(void*,CVPixelBufferRef,int64_t,int64_t,void(*)(void*,int64_t),void*);
} KLJMediaSurface;
void klj_media_surface_install(int(*javaSurface)(JNIEnv*,jobject,KLJMediaSurface*),int(*nativeWindow)(void*,KLJMediaSurface*));
int klj_media_surface_acquire(void*,KLJMediaSurface*);
int klj_codec_surface_java(JNIEnv*,jobject,KLJMediaSurface*);
int klj_media_reader_surface(void*,KLJMediaSurface*);
#ifdef __cplusplus
}
#endif
