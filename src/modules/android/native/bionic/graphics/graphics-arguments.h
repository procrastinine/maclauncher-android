#include "graphics-host.h"
#include <android/bitmap.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/hardware_buffer_aidl.h>
typedef struct {AHardwareBuffer * a0;} GraphicsArgs_0;
typedef struct {const AHardwareBuffer_Desc * a0;AHardwareBuffer * _Nullable * a1;int result;} GraphicsArgs_1;
typedef struct {const AHardwareBuffer * a0;AHardwareBuffer_Desc * a1;} GraphicsArgs_2;
typedef struct {JNIEnv * a0;jobject a1;AHardwareBuffer * result;} GraphicsArgs_3;
typedef struct {const AHardwareBuffer * a0;uint64_t * a1;int result;} GraphicsArgs_4;
typedef struct {const AHardwareBuffer_Desc * a0;int result;} GraphicsArgs_5;
typedef struct {AHardwareBuffer * a0;uint64_t a1;int32_t a2;const ARect * a3;void * _Nullable * a4;int result;} GraphicsArgs_6;
typedef struct {AHardwareBuffer * a0;uint64_t a1;int32_t a2;const ARect * a3;void * _Nullable * a4;int32_t * a5;int32_t * a6;int result;} GraphicsArgs_7;
typedef struct {AHardwareBuffer * a0;uint64_t a1;int32_t a2;const ARect * a3;AHardwareBuffer_Planes * a4;int result;} GraphicsArgs_8;
typedef struct {const AParcel * a0;AHardwareBuffer * _Nullable * a1;binder_status_t result;} GraphicsArgs_9;
typedef struct {int a0;AHardwareBuffer * _Nullable * a1;int result;} GraphicsArgs_10;
typedef struct {AHardwareBuffer * a0;} GraphicsArgs_11;
typedef struct {const AHardwareBuffer * a0;int a1;int result;} GraphicsArgs_12;
typedef struct {JNIEnv * a0;AHardwareBuffer * a1;jobject result;} GraphicsArgs_13;
typedef struct {AHardwareBuffer * a0;int32_t * a1;int result;} GraphicsArgs_14;
typedef struct {const AHardwareBuffer * a0;AParcel * a1;binder_status_t result;} GraphicsArgs_15;
typedef struct {const AndroidBitmapInfo * a0;int32_t a1;const void * a2;int32_t a3;int32_t a4;void * a5;AndroidBitmap_CompressWriteFunc a6;int result;} GraphicsArgs_16;
typedef struct {JNIEnv * a0;jobject a1;int32_t result;} GraphicsArgs_17;
typedef struct {JNIEnv * a0;jobject a1;AHardwareBuffer ** a2;int result;} GraphicsArgs_18;
typedef struct {JNIEnv * a0;jobject a1;AndroidBitmapInfo * a2;int result;} GraphicsArgs_19;
typedef struct {JNIEnv * a0;jobject a1;void ** a2;int result;} GraphicsArgs_20;
typedef struct {JNIEnv * a0;jobject a1;int result;} GraphicsArgs_21;
