#pragma once
#include <jni.h>
#include <stddef.h>
#include <stdint.h>
#include "../image-format.h"
#ifdef __cplusplus
extern "C" {
#endif
// NativeBinding retains this process owner for the JVM lifetime, just like the
// ARM64 loader. Addresses remain opaque outside their architecture's adapter.
typedef struct {
  uint32_t version;
  void *(*create)(JNIEnv *, void *(*lookup)(const char *), void *(*guest_symbol)(const char *));
  const KlImageFormat *(*format)(void *process);
  int (*bind)(JNIEnv *, void *image, jclass target, const char *const *names,
              const char *const *descriptors, const char *const *short_names,
              const char *const *long_names, size_t count);
  void (*initialize)(JNIEnv *, void *image);
  void *(*create_metadata)(JNIEnv *,void *(*lookup)(const char *),void *(*guest_symbol)(const char *),
                          const char *(*method_shorty)(JNIEnv *,jmethodID));
  void *(*trampoline)(void *process,uint32_t function,const char *shorty,size_t length,unsigned call_type);
  int (*executable)(void *process,uintptr_t function);
  uintptr_t (*caller)(void *process);
} klj_arm32_runtime_api;
const klj_arm32_runtime_api *klj_arm32_runtime(void);
#ifdef __cplusplus
}
#endif
