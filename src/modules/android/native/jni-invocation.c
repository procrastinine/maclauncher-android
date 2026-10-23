#include "jni-invocation.h"
#include <string.h>
#include <stdatomic.h>
#include <stddef.h>
#include <setjmp.h>
#include <pthread.h>

_Static_assert(sizeof(KljJniInvocation)==64,"JNI assembly invocation storage");
_Static_assert(offsetof(KljJniInvocation,exitContext)==40,"JNI assembly exit continuation");
_Static_assert(sizeof(jmp_buf)<=192,"JNI assembly continuation storage");
static _Thread_local KljJniInvocation *current;
static _Thread_local unsigned vm_retirement;
static _Thread_local unsigned metadata_attachment;
static _Thread_local KljJniAttachScope *attachment;
static _Atomic(const KljJniThreadLifecycle *) thread_lifecycle;
static _Atomic(KljJniThreadObserver *) retirement_observers;
static pthread_mutex_t retirement_mutex=PTHREAD_MUTEX_INITIALIZER;
int klj_jni_thread_observe(KljJniThreadObserver *owner){
    if(!owner||(!owner->javaEnd&&!owner->physicalEnd))return 0;
    pthread_mutex_lock(&retirement_mutex);KljJniThreadObserver *head=atomic_load_explicit(&retirement_observers,memory_order_acquire);
    for(KljJniThreadObserver *entry=head;entry;entry=entry->next)if(entry==owner){pthread_mutex_unlock(&retirement_mutex);return 1;}
    owner->next=head;atomic_store_explicit(&retirement_observers,owner,memory_order_release);pthread_mutex_unlock(&retirement_mutex);return 1;
}
void klj_jni_thread_lifecycle(const KljJniThreadLifecycle *owner){atomic_store_explicit(&thread_lifecycle,owner,memory_order_release);}
void klj_jni_thread_retire(void){
    if(metadata_attachment)return;
    ++vm_retirement;
    for(KljJniThreadObserver *entry=atomic_load_explicit(&retirement_observers,memory_order_acquire);entry;entry=entry->next)if(entry->javaEnd)entry->javaEnd(entry->owner);
    const KljJniThreadLifecycle *owner=atomic_load_explicit(&thread_lifecycle,memory_order_acquire);
    if(owner&&owner->javaEnd)owner->javaEnd();
    --vm_retirement;
}
jint klj_jni_borrow_attachment(JavaVM *vm,KljJniBorrowedAttachment *scope){
    if(!vm||!scope)return JNI_EINVAL;
    *scope=(KljJniBorrowedAttachment){.vm=vm};
    jint status=(*vm)->GetEnv(vm,(void**)&scope->environment,JNI_VERSION_1_6);
    if(status==JNI_OK)return status;
    if(status!=JNI_EDETACHED)return status;
    metadata_attachment++;
    status=(*vm)->AttachCurrentThreadAsDaemon(vm,(void**)&scope->environment,NULL);
    if(status!=JNI_OK||!scope->environment){
        if(status==JNI_OK)(void)(*vm)->DetachCurrentThread(vm);
        metadata_attachment--;*scope=(KljJniBorrowedAttachment){0};
        return status==JNI_OK?JNI_ERR:status;
    }
    scope->borrowed=1;return JNI_OK;
}
jint klj_jni_return_attachment(KljJniBorrowedAttachment *scope){
    if(!scope||!scope->vm)return JNI_EINVAL;
    jint status=JNI_OK;
    if(scope->borrowed){
        status=(*scope->vm)->DetachCurrentThread(scope->vm);
        metadata_attachment--;
    }
    *scope=(KljJniBorrowedAttachment){0};return status;
}
int klj_jni_thread_retiring(void){return vm_retirement!=0;}
void klj_jni_physical_thread_retire(void){
    for(KljJniThreadObserver *entry=atomic_load_explicit(&retirement_observers,memory_order_acquire);entry;entry=entry->next)if(entry->physicalEnd)entry->physicalEnd(entry->owner);
    const KljJniThreadLifecycle *owner=atomic_load_explicit(&thread_lifecycle,memory_order_acquire);
    if(owner&&owner->physicalEnd)owner->physicalEnd();
}
jint klj_jni_detach_current_thread(JavaVM *vm){
    if(vm_retirement)return JNI_OK;
    ++vm_retirement;
    jint result=(*vm)->DetachCurrentThread(vm);
    --vm_retirement;
    return result;
}

int klj_jni_invocation_enter(KljJniInvocation *scope,JNIEnv *env){
    *scope=(KljJniInvocation){.previous=current,.environment=env};current=scope;
    const KljJniThreadLifecycle *owner=atomic_load_explicit(&thread_lifecycle,memory_order_acquire);
    int error=owner&&owner->enter?owner->enter():0;
    if(error&&env&&!(*env)->ExceptionCheck(env)){
        jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
        if(type)(*env)->ThrowNew(env,type,"Android native thread could not initialize");
    }
    return error==0;
}
KljJniInvocation *klj_jni_current_invocation(void){return current;}
void klj_jni_defer_thread_exit(KljJniInvocation *scope){scope->exitDeferred=1;}
int klj_jni_thread_exit_requested(void){return current&&current->exitRequested;}
int klj_jni_request_thread_exit(void){
    KljJniInvocation *scope=current;
    /* A suspended scope has a live Java callback above it. Only that callback's
     * own native entry can return to the VM; never jump across those frames. */
    if(!scope||scope->suspended||scope->internal||(!scope->exitContext&&!scope->exitDeferred))return 0;
    JNIEnv *env=scope->environment;
    if(!(*env)->ExceptionCheck(env)){
        jclass type=(*env)->FindClass(env,"java/lang/ThreadDeath");
        if(type){
            jmethodID constructor=(*env)->GetMethodID(env,type,"<init>","()V");
            jthrowable death=constructor?(*env)->NewObjectA(env,type,constructor,NULL):NULL;
            if(death){(*env)->Throw(env,death);(*env)->DeleteLocalRef(env,death);}
            (*env)->DeleteLocalRef(env,type);
        }
    }
    scope->exitRequested=1;
    if(scope->exitContext)_longjmp(scope->exitContext,1);
    return 1;
}
int klj_jni_invocation_attach(void *resource,const KljJniInvocationResource *operations){
    if(!current||current->resource||!resource||!operations)return 0;
    current->resource=resource;current->operations=operations;return 1;
}
static int synchronize(KljJniInvocation *scope,JNIEnv *env,int toJava){
    if(!scope->resource)return 1;
    /* Cleanup and synchronization also occur on exceptional JNI returns. Keep
     * the original pending throwable while using normal JNI to retain owners. */
    jthrowable pending=(*env)->ExceptionOccurred(env);
    if(pending)(*env)->ExceptionClear(env);
    scope->internal++;
    int result=scope->operations->synchronize(scope->resource,env,toJava);
    scope->internal--;
    if(pending){
        if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
        (*env)->Throw(env,pending);(*env)->DeleteLocalRef(env,pending);
    }
    return result;
}
int klj_jni_before_java(JNIEnv *env){
    KljJniInvocation *scope=current;if(!scope||scope->internal)return 1;
    if(scope->suspended++){return 1;}
    if(synchronize(scope,env,1))return 1;
    scope->suspended--;return 0;
}
void klj_jni_after_java(JNIEnv *env){
    KljJniInvocation *scope=current;if(!scope||scope->internal||!scope->suspended)return;
    if(--scope->suspended==0)(void)synchronize(scope,env,0);
}
void klj_jni_invocation_leave(KljJniInvocation *scope){
    if(current!=scope)return;
    JNIEnv *env=scope->environment;
    if(scope->resource){
        if(!scope->suspended)(void)synchronize(scope,env,1);
        jthrowable pending=(*env)->ExceptionOccurred(env);if(pending)(*env)->ExceptionClear(env);
        scope->internal++;
        scope->operations->destroy(scope->resource,env);
        scope->internal--;
        if(pending){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,pending);(*env)->DeleteLocalRef(env,pending);}
    }
    current=scope->previous;
}
const KljJniInvocationApi *klj_jni_invocation_api(void){
    static const KljJniInvocationApi api={klj_jni_invocation_enter,klj_jni_invocation_leave,klj_jni_before_java,klj_jni_after_java,klj_jni_defer_thread_exit,klj_jni_thread_exit_requested,klj_jni_thread_retiring,klj_jni_detach_current_thread,klj_jni_physical_thread_retire};
    return &api;
}

void klj_jni_attach_scope_enter(KljJniAttachScope *scope,int(*prepare)(void *),void *owner){
    *scope=(KljJniAttachScope){.previous=attachment,.owner=owner,.prepare=prepare};attachment=scope;
}
void klj_jni_attach_scope_leave(KljJniAttachScope *scope){if(attachment==scope)attachment=scope->previous;}
int klj_jni_attach_prepare(void){return !attachment?0:(!attachment->prepare||!attachment->prepare(attachment->owner)?1:-1);}
int klj_jni_invocation_enter_owned(KljJniInvocation *scope,JNIEnv *env,int(*enter)(void *),void *owner){
    *scope=(KljJniInvocation){.previous=current,.environment=env,.exitDeferred=1};current=scope;
    int error=enter?enter(owner):0;
    if(error&&env&&!(*env)->ExceptionCheck(env)){
        jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
        if(type)(*env)->ThrowNew(env,type,"Android native thread could not initialize");
    }
    return error==0;
}
