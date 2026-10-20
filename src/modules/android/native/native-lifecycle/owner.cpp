#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "thread-local.h"

extern "C" int klj_bionic_cxa_atexit(void (*)(void*), void*, void*);
extern "C" void klj_bionic_cxa_finalize(void*);
extern "C" int klj_bionic_register_atfork(void (*)(void), void (*)(void), void (*)(void), void*);
void __bionic_atfork_run_prepare();
void __bionic_atfork_run_parent();
void __bionic_atfork_run_child();
extern "C" void kl_thread_init(void);
extern "C" void klj_enter_guest_thread(void);
extern "C" KlGuestThreadLocalDestructors* klj_guest_thread_local_storage() {
    klj_enter_guest_thread();
    static thread_local KlGuestThreadLocalDestructors state{};
    return &state;
}

extern "C" void klj_guest_stdio_cleanup() { fflush(nullptr); }
extern "C" void klj_guest_cxa_finalize(void* dso) {
    kl_thread_init();
    klj_bionic_cxa_finalize(dso);
}
extern "C" int klj_guest_cxa_atexit(void (*fn)(void*), void* argument, void* dso) {
    return klj_bionic_cxa_atexit(fn, argument, dso);
}
// Android process retirement is not guest libc exit. An Activity or JVM can
// retire after a game has already released its allocator and engine globals.
// Only an explicit guest exit/finalize request runs the guest destructor table.
extern "C" void klj_bionic_exit(int);
extern "C" void klj_guest_exit(int status) {
    kl_thread_init();
    klj_bionic_exit(status);
}
static void exitCallback(void* fn) { reinterpret_cast<void(*)(void)>(fn)(); }
extern "C" int klj_guest_atexit(void (*fn)(void)) {
    return klj_guest_cxa_atexit(exitCallback, reinterpret_cast<void*>(fn), nullptr);
}

// Darwin performs the actual fork; Bionic retains the complete callback order
// and DSO-removal semantics. One registration connects the two owners.
static pthread_once_t forkOnce = PTHREAD_ONCE_INIT;
static int forkRegistration;
static void registerFork() {
    forkRegistration = pthread_atfork(__bionic_atfork_run_prepare,
        __bionic_atfork_run_parent, __bionic_atfork_run_child);
}
extern "C" int klj_guest_register_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void), void* dso) {
    pthread_once(&forkOnce, registerFork);
    if (forkRegistration) return forkRegistration;
    return klj_bionic_register_atfork(prepare, parent, child, dso);
}
extern "C" int klj_guest_pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    return klj_guest_register_atfork(prepare, parent, child, nullptr);
}
