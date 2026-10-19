#include "clock-domain.h"
#include <jni.h>
int klj_android_monotonic_contract_v1(void) { return 1; }
JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_Clock_bootTimeNanos(JNIEnv *env, jclass cls) {
    (void)cls; struct timespec value;
    if (klj_clock_read(7, &value)) {
        jclass error = (*env)->FindClass(env, "java/lang/IllegalStateException");
        if (error) (*env)->ThrowNew(env, error, "Unable to read elapsed realtime");
        return 0;
    }
    return (jlong)((uint64_t)value.tv_sec * 1000000000ull + value.tv_nsec);
}
