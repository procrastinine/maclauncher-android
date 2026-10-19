#pragma once

// Bionic retains its complete destructor list and drain algorithm. Only its
// pthread_internal_t storage slot belongs to the physical macOS thread owner.
class thread_local_dtor;
struct KlGuestThreadLocalDestructors {
    thread_local_dtor* thread_local_dtors;
};
extern "C" KlGuestThreadLocalDestructors* klj_guest_thread_local_storage();

#define pthread_internal_t KlGuestThreadLocalDestructors
#define __get_thread klj_guest_thread_local_storage
#define __cxa_thread_atexit_impl klj_guest_cxa_thread_atexit_impl
#define __cxa_thread_finalize klj_guest_cxa_thread_finalize
#define __BIONIC_WEAK_FOR_NATIVE_BRIDGE
#define __LIBC_HIDDEN__ __attribute__((visibility("hidden")))

// Guest images remain owned by the process image registry for their lifetime.
// These optional original linker hooks retain their weak-import semantics.
#define __loader_add_thread_local_dtor klj_guest_loader_add_thread_local_dtor
#define __loader_remove_thread_local_dtor klj_guest_loader_remove_thread_local_dtor
