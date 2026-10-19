#pragma once
// Android's GLES wrapper ABI adds an extent to the original GL entrypoints.
// ANGLE owns all GL state; the original JNI caller owns the array validation.
#ifdef __cplusplus
namespace android { int egl_get_init_count(void *display); }
extern "C" {
#else
int klj_android_egl_get_init_count(void *display) __asm__("_ZN7android18egl_get_init_countEPv");
#endif
void glColorPointerBounds(int,unsigned,int,const void*,int);
void glTexCoordPointerBounds(int,unsigned,int,const void*,int);
void glVertexPointerBounds(int,unsigned,int,const void*,int);
void glMatrixIndexPointerOESBounds(int,unsigned,int,const void*,int);
void glWeightPointerOESBounds(int,unsigned,int,const void*,int);
void glNormalPointerBounds(unsigned,int,const void*,int);
void glPointSizePointerOESBounds(unsigned,int,const void*,int);
#ifdef __cplusplus
}
#endif
