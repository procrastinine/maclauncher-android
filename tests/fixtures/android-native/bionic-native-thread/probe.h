#pragma once
#include <jni.h>
#include <stdint.h>

enum { PROBE_NDK=1, PROBE_GRAPHICS=2, PROBE_EGL=4, PROBE_ALL=7 };
typedef struct {
 JavaVM *vm;
 unsigned groups;
 int line,callback_line,callbacks,before,inside,after;
 int retired_read,retired_write;
 int32_t owner_tid,worker_tid;
 uintptr_t worker_pthread;
} NativeThreadCall;
