#include <jni.h>
#include <stdatomic.h>
#include <stdio.h>

/* Android sends this advisory event to its graphics performance service.
 * macOS schedules the existing Metal workload directly; there is no Android
 * GPU service to receive a launch boost request. Keep the original lifecycle
 * operational without claiming that a boost was applied. */
JNIEXPORT void JNICALL Java_android_os_GraphicsEnvironment_hintActivityLaunch(JNIEnv *env,jclass type){
    (void)env;(void)type;
    static atomic_flag reported=ATOMIC_FLAG_INIT;
    if(!atomic_flag_test_and_set_explicit(&reported,memory_order_relaxed))
        fprintf(stderr,"[android-host] Android graphics launch hints have no Mac service; Metal uses the system scheduler.\n");
}
