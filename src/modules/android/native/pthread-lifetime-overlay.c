// The original guest destructors run while the host JVM's thread-local
// attachment still exists. Darwin's later TSD pass sees the cleared guest slot.
// Entered by guest workers and host audio/media callback threads alike.
#include <errno.h>
#include <jni.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "jni-invocation.h"
#include "executable-memory/jvm.h"
#include "android-log.h"

// The loader publishes the process JVM before guest constructors can start
// workers. Guest native-thread ownership ends only after its original key
// destructors have run; JVM attachment must end before the native thread does.
static _Atomic(JavaVM *) klj_guest_thread_vm;
static _Thread_local int klj_guest_lifetime; // 0 unseen, 1 Java-owned, 2 owned, 3 retiring
extern void kl_thread_init(void);
void klj_finish_guest_thread(void);
extern void klj_guest_cxa_thread_finalize(void);
static int klj_enter_thread_lifetime(void);
static void klj_java_thread_end(void);
static const KljJniThreadLifecycle klj_thread_lifecycle = {
    klj_enter_thread_lifetime, klj_java_thread_end, klj_finish_guest_thread
};
static void klj_guest_lifetime_exit(void *owner) {
    (void)owner;
    klj_finish_guest_thread();
}
void klj_set_guest_thread_vm(JavaVM *vm) {
    klj_android_log_initialize();
    atomic_store_explicit(&klj_guest_thread_vm,vm,memory_order_release);
    klj_jni_thread_lifecycle(&klj_thread_lifecycle);
}
JNIEnv *klj_guest_thread_environment(void) {
    JavaVM *vm=atomic_load_explicit(&klj_guest_thread_vm,memory_order_acquire);
    JNIEnv *env=NULL;
    return vm&&(*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)==JNI_OK?env:NULL;
}
void klj_enter_guest_thread(void) {
    JavaVM *vm=atomic_load_explicit(&klj_guest_thread_vm,memory_order_acquire);
    if(!vm)return;
    // The existing Mach-thread owner retains its native execution context
    // through late JVM/POSIX cleanup; no second thread registry is created.
    (void)klj_executable_thread_enter();
    if(klj_guest_lifetime)return;
    JNIEnv *env=NULL;
    klj_guest_lifetime=(*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)==JNI_OK?1:2;
    // Darwin's compiler TLS retirement precedes ordinary pthread-key cleanup.
    // A pthread-key destructor is too late: HotSpot's own key can already be
    // cleared even though its JavaThread record still refers to this thread.
    _tlv_atexit(klj_guest_lifetime_exit,NULL);
}
static int klj_enter_thread_lifetime(void) {
    // Every JNI entry borrows the same complete Android thread setup as a
    // native worker: retained context, thread pointer and original canary.
    // Merely retaining the context leaves the rewritten TP load unset.
    kl_thread_init();
    return 0;
}
static void klj_java_thread_end(void) {
    // Detaching an attached native worker does not end its physical thread.
    // Its C++/POSIX state belongs to that worker's later return/exit cleanup.
    if(klj_guest_lifetime==1)klj_finish_guest_thread();
}
static int klj_begin_guest_thread_retirement(void) {
    if(klj_guest_lifetime!=1&&klj_guest_lifetime!=2)return 0;
    int native_owned=klj_guest_lifetime==2;
    klj_guest_lifetime=3;
    return native_owned?2:1;
}
static void klj_detach_guest_thread(void) {
    JavaVM *vm=atomic_load_explicit(&klj_guest_thread_vm,memory_order_acquire);
    JNIEnv *env=NULL;
    if(vm&&(*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)==JNI_OK &&
       klj_jni_detach_current_thread(vm)!=JNI_OK)
        fprintf(stderr,"[android-native] JVM attachment could not be released at native thread exit\n");
}

// Android native workers can enter the JVM at any point. Keep the native host's
// default stack reserve when Android requests a smaller one: HotSpot needs its
// guard/shadow pages before AttachCurrentThread can succeed. This is virtual
// address space, not eagerly committed memory. Larger guest reserves survive.
static int klj_pthread_set_guest_stacksize(pthread_attr_t *attributes,size_t requested) {
    size_t native_default=0;
    int error=pthread_attr_getstacksize(attributes,&native_default);
    if(error)return error;
    size_t page=(size_t)getpagesize();
    if(requested<native_default)requested=native_default;
    if(requested>SIZE_MAX-(page-1))return EINVAL;
    requested=(requested+page-1)/page*page;
    return pthread_attr_setstacksize(attributes,requested);
}
#include KLEPTON_PTHREAD_LIFETIME_SOURCE
