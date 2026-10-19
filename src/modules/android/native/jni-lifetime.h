#pragma once
#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif
enum { KLJ_JNI_SHUTDOWN = 1, KLJ_JNI_VM_DEATH = 2 };
/* Component records have native-library lifetime. Ordinary shutdown drains
 * producers while Java can run; VMDeath only seals the terminal boundary. */
typedef struct KljJniVmObserver {
    void (*retire)(JNIEnv *,int);
    struct KljJniVmObserver *next;
} KljJniVmObserver;
int klj_jni_vm_observe(JNIEnv *,KljJniVmObserver *);
JNIEnv *klj_jni_vm_environment(JavaVM *);
#ifdef __cplusplus
}
#endif
