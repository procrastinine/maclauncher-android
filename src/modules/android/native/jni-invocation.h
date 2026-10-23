#pragma once
#include <jni.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* One real VM and its Android ABI facade are published by the process loader.
 * Cold invocation-API queries and platform callbacks borrow that same owner. */
int klj_jni_publish_vm(JavaVM *actual,JavaVM *guest);
JavaVM *klj_jni_actual_vm(void);
void klj_jni_clear_vm(JavaVM *actual);
jint klj_jni_get_created_vms(JavaVM **buffer,jsize capacity,jsize *count);
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
/* Returning foreign providers defer native exit until their original dispatcher
 * and host-import cleanup have returned; no setjmp continuation is installed. */
int klj_jni_invocation_enter_owned(KljJniInvocation *,JNIEnv *,int(*)(void *),void *);
typedef struct {void *previous;void *owner;int(*prepare)(void *);} KljJniAttachScope;
void klj_jni_attach_scope_enter(KljJniAttachScope *,int(*)(void *),void *);
void klj_jni_attach_scope_leave(KljJniAttachScope *);
int klj_jni_attach_prepare(void);
/* Internal metadata may borrow JNI on a live native thread. Its temporary
 * JavaThread attachment owns none of that physical thread's Android state. */
typedef struct {JavaVM *vm;JNIEnv *environment;unsigned borrowed;} KljJniBorrowedAttachment;
jint klj_jni_borrow_attachment(JavaVM *,KljJniBorrowedAttachment *);
jint klj_jni_return_attachment(KljJniBorrowedAttachment *);
/* Borrowed immutable operations from the initialized original process owner.
 * ThreadEnd retires foreign Android TLS while the real JVM environment lives. */
typedef struct {int (*enter)(void);void (*javaEnd)(void);void (*physicalEnd)(void);} KljJniThreadLifecycle;
void klj_jni_thread_lifecycle(const KljJniThreadLifecycle *);
/* Additional VM-lifetime providers retire while the real JNIEnv still exists;
 * they never replace the legacy thread owner or install another JVMTI agent. */
typedef struct KljJniThreadObserver {
    void (*javaEnd)(void *);void (*physicalEnd)(void *);void *owner;
    struct KljJniThreadObserver *next;
} KljJniThreadObserver;
int klj_jni_thread_observe(KljJniThreadObserver *);
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
