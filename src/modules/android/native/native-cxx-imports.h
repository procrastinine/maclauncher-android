#ifndef MACLAUNCHER_NATIVE_CXX_IMPORTS_H
#define MACLAUNCHER_NATIVE_CXX_IMPORTS_H
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

// ARM64 Android and Darwin use the same 64-bit Itanium guard ABI. Reuse
// libc++abi's synchronization, waiters and abort/retry instead of inventing
// per-library locks. The loader checks guest definitions before this fallback.
static pthread_once_t klj_cxx_once=PTHREAD_ONCE_INIT;
static void *klj_cxx_guards[3];
static void klj_cxx_init(void) {
    // JNI libraries may be loaded RTLD_LOCAL. Resolve the system ABI explicitly
    // and retain this one handle for the lifetime of its imported function pointers.
    void *library=dlopen("/usr/lib/libc++abi.dylib",RTLD_LAZY|RTLD_LOCAL);
    if(!library)return;
    klj_cxx_guards[0]=dlsym(library,"__cxa_guard_acquire");
    klj_cxx_guards[1]=dlsym(library,"__cxa_guard_release");
    klj_cxx_guards[2]=dlsym(library,"__cxa_guard_abort");
}
static void *klj_native_cxx_lookup(const char *name) {
    int index=!strcmp(name,"__cxa_guard_acquire")?0:!strcmp(name,"__cxa_guard_release")?1:
        !strcmp(name,"__cxa_guard_abort")?2:-1;
    if(index<0)return NULL;
    pthread_once(&klj_cxx_once,klj_cxx_init);
    return klj_cxx_guards[index];
}
#endif
