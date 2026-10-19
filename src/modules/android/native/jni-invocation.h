#pragma once
#include <jni.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Stack ownership around an original native invocation. Resources are attached
 * lazily; ordinary JNI calls allocate neither transport nor resource records. */
typedef struct KljJniInvocation KljJniInvocation;
typedef struct {
    int (*synchronize)(void *,JNIEnv *,int toJava);
    void (*destroy)(void *,JNIEnv *);
} KljJniInvocationResource;
struct KljJniInvocation {
    KljJniInvocation *previous;
    JNIEnv *environment;
    void *resource;
    const KljJniInvocationResource *operations;
    unsigned internal,suspended;
    void *exitContext;
    unsigned exitRequested,exitDeferred;
    uintptr_t reserved;
};
int klj_jni_invocation_enter(KljJniInvocation *,JNIEnv *);
/* Borrowed immutable operations from the initialized original process owner.
 * ThreadEnd retires foreign Android TLS while the real JVM environment lives. */
typedef struct {int (*enter)(void);void (*javaEnd)(void);void (*physicalEnd)(void);} KljJniThreadLifecycle;
void klj_jni_thread_lifecycle(const KljJniThreadLifecycle *);
void klj_jni_thread_retire(void);
void klj_jni_physical_thread_retire(void);
/* The VM owns attachment retirement. A callback made by its ThreadEnd event
 * can request the same detach without recursively destroying that JavaThread. */
int klj_jni_thread_retiring(void);
jint klj_jni_detach_current_thread(JavaVM *);
void klj_jni_invocation_leave(KljJniInvocation *);
int klj_jni_before_java(JNIEnv *);
void klj_jni_after_java(JNIEnv *);
KljJniInvocation *klj_jni_current_invocation(void);
int klj_jni_invocation_attach(void *,const KljJniInvocationResource *);
/* A native guest can end a VM-owned thread only by returning through its JNI
 * entry. The interpreter defers that return until its host-import RAII ends;
 * ARM64 uses the native entry's stack continuation. Neither crosses Java. */
int klj_jni_request_thread_exit(void);
void klj_jni_defer_thread_exit(KljJniInvocation *);
int klj_jni_thread_exit_requested(void);
typedef struct {
    int (*enter)(KljJniInvocation *,JNIEnv *);
    void (*leave)(KljJniInvocation *);
    int (*beforeJava)(JNIEnv *);
    void (*afterJava)(JNIEnv *);
    void (*deferExit)(KljJniInvocation *);
    int (*exitRequested)(void);
    int (*retiring)(void);
    jint (*detach)(JavaVM *);
    void (*physicalEnd)(void);
} KljJniInvocationApi;
const KljJniInvocationApi *klj_jni_invocation_api(void);
#ifdef __cplusplus
}
#endif
