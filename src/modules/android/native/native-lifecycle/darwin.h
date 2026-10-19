#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// Bionic's guest DSO handles are independent of Mach-O/JVM DSO handles. Keep
// the upstream finalization algorithm without finalizing the host's own C++
// objects when a guest calls __cxa_finalize(nullptr).
#define __cxa_atexit klj_bionic_cxa_atexit
#define __cxa_finalize klj_bionic_cxa_finalize
#define __register_atfork klj_bionic_register_atfork
#define __unregister_atfork klj_bionic_unregister_atfork
#define __libc_stdio_cleanup klj_guest_stdio_cleanup
#define __LIBC_HIDDEN__ __attribute__((visibility("hidden")))
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#define BIONIC_DISALLOW_COPY_AND_ASSIGN(Type) Type(const Type&) = delete; Type& operator=(const Type&) = delete

static inline uintptr_t page_start(uintptr_t value) { return value & ~(uintptr_t(getpagesize()) - 1); }
static inline uintptr_t page_end(uintptr_t value) { return page_start(value + getpagesize() - 1); }
extern "C" void* kl_shim_lookup(const char*);
static inline void* klj_lifecycle_mremap(void* address, size_t oldSize, size_t newSize, int flags) {
    using Remap = void*(*)(void*, size_t, size_t, int, ...);
    return reinterpret_cast<Remap>(kl_shim_lookup("mremap"))(address, oldSize, newSize, flags);
}
#define mremap klj_lifecycle_mremap
#define MREMAP_MAYMOVE 1
#define ANDROID_LOG_WARN 5
#define async_safe_format_log(priority, tag, ...) do { fprintf(stderr, "%s: ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define async_safe_fatal(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); abort(); } while (0)
