#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same-process guest mapping/protection boundary. The physical VM executor
 * serializes mapping mutation before calling. The original AIM MAP_JIT owner
 * preserves resident bytes when entering or leaving an executable allocation.
 * Returns zero or a negative Linux errno; the caller's native errno is kept.
 * This API does not install a signal handler or own a VM/thread context. */
int64_t klj_guest_executable_protect(uintptr_t address, size_t length, int protection);
/* The executor translates private staging bytes using their final address.
 * A failed preparation leaves the original mapping and permissions intact. */
typedef int64_t (*KljPrepareExecutable)(uintptr_t,void *,size_t);
typedef void (*KljFinishExecutable)(uintptr_t,void *,size_t,int);
int64_t klj_guest_executable_protect_prepared(uintptr_t,size_t,int,KljPrepareExecutable,KljFinishExecutable);
/* Kernel VM-tag ownership query for the existing physical signal dispatcher.
 * Foreign VM/JIT allocations never acquire Android ownership. */
int klj_guest_executable_contains(uintptr_t address);
/* Original AIM MRS/MSR TP body using the executor's allocated Darwin key.
 * Returns its word count or a negative Linux errno. No branch is appended. */
int64_t klj_guest_thread_pointer_stub(uint32_t instruction,uint32_t slot_offset,
                                    uint32_t shadow_offset,uint32_t *output,size_t capacity);

/* Per-physical-thread storage and guarded resume stack belong to the existing
 * executor. No fixed pthread keys, signal handlers or process registries are
 * installed. The executor offers hardware faults to this boundary before its
 * usual VM/guest classification, and otherwise retains normal signal handling. */
size_t klj_guest_executable_context_size(void);
size_t klj_guest_executable_context_alignment(void);
void klj_guest_executable_native_stack(void *, unsigned long long **, unsigned int **);
int64_t klj_guest_executable_context_init(void *storage, size_t storage_size,
                                        uintptr_t stack_start, size_t stack_size);
int klj_guest_executable_fault(void *storage, int signal, int code, void *ucontext);

/* Complete original Android signal frame over the native callback's lifetime.
 * The existing signal owner converts mask numbers at its platform boundary. */
typedef struct {void *frame;void *info;void *context;} KljGuestSignalFrame;
size_t klj_guest_signal_frame_size(void);
int64_t klj_guest_signal_frame_prepare(void *storage, size_t size,
                                     const void *linux_siginfo, void *ucontext,
                                     uint64_t mask, KljGuestSignalFrame *output);
int64_t klj_guest_signal_frame_restore(const void *frame, void *ucontext,
                                     uint64_t *returned_mask);
/* Chaining a saved native disposition crosses the same ABI in reverse. The
 * Linux context may be a caller-owned copy, not the original frame address. */
int64_t klj_guest_signal_context_restore(const void *context, void *ucontext,
                                       uint64_t *returned_mask);
int64_t klj_guest_signal_context_update(void *context, void *ucontext,
                                      uint64_t mask);
/* Original Linux kernel action record and AIM disposition owner. Linux
 * realtime signals have no Darwin signal number. */
typedef struct {uintptr_t handler;uint64_t flags;uintptr_t restorer;uint64_t mask;} KljGuestSignalAction;
int64_t klj_guest_signal_action(int signal,const KljGuestSignalAction *action,
                              KljGuestSignalAction *previous);

#ifdef __cplusplus
}
#endif
