#ifndef KLJ_NEXT_GLES_COMMON_H
#define KLJ_NEXT_GLES_COMMON_H
#include <jni.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void klj_next_set_resolver(void*(*resolver)(const char*));
void klj_next_throw(JNIEnv*,const char*className,const char*message);
void* klj_next_symbol(JNIEnv*,const char*name);
/* A NULL result may be valid for zero bytes; check ExceptionCheck. */
void* klj_next_buffer(JNIEnv*,jobject,int64_t requiredBytes,int output);
int klj_next_array_range(JNIEnv*,jarray,jint offset,jint count);
int klj_next_query_count(JNIEnv*,unsigned parameter);
int64_t klj_next_uniform_components(JNIEnv*,unsigned program,int location);
/* storage flags: bit0 pack/output, bit1 3D transfer (even when depth=1). */
int64_t klj_next_pixel_bytes(JNIEnv*,int width,int height,int depth,unsigned format,unsigned type,int storage);
#ifdef __cplusplus
}
#endif
#endif
