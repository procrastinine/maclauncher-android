#pragma once
#include <mach/mach.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Linux-sized IDs for Android callers; Darwin kernel IDs remain host-private. */
int klj_thread_id(void);
/* Adopt a lease reserved before pthread_create. The same retained Mach-thread
 * owner retires adopted and lazily registered leases after kernel death. */
int klj_thread_id_adopt(int);
int klj_thread_id_reserve(void);
/* Hold an owned reserved or adopted TID. A pending RAII owner holds before
 * publishing its clone, then keeps that hold through adoption or tracer wait.
 * Dropping the last pending hold cancels its lease. Physical resources retire
 * at Mach death; the last hold releases the ID only after that death.
 * Both operations return the ID, or -1 with errno. Holds may be nested. */
int klj_thread_id_hold(int);
int klj_thread_id_unhold(int);
/* Optional executor context belongs to this retained physical thread. It is
 * retired only after kernel thread death, including later POSIX destructors. */
void *klj_thread_memory_context(void);
int klj_thread_memory_adopt(void *context, void (*retire)(void *));
/* Publish auxiliary state under the same retirement/fork lock as adoption.
 * prepare returns zero on success and can set errno on failure. */
int klj_thread_memory_adopt_prepared(void *,void (*retire)(void *),int (*prepare)(void *));
/* A retained right to a live thread in this process. Caller deallocates it. */
thread_t klj_thread_acquire(int);
/* Priority APIs return Darwin errno values; JNI/libc translate at their ABI. */
int klj_thread_priority_set(int, int);
int klj_thread_priority_get(int, int *);
int klj_thread_priority_target(int);
/* Linux scheduling policy and sched_param representation; errors are Darwin errno. */
int klj_thread_scheduler_get(int, int *, int *, long *);
int klj_thread_scheduler_set(int, int, int);
#ifdef __cplusplus
}
#endif
