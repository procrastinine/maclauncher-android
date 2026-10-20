#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Original guest-thread entry and signal multiplexer use these same-process
 * hooks. Physical retirement stays with the existing Mach-thread owner. */
int klj_executable_thread_enter(void);
/* This executor's original Android TLS slots live with its retained physical
 * thread, reached through an allocated host key rather than reserved TSD. */
int klj_guest_thread_pointer_offset(void);
int klj_guest_thread_pointer_enter(unsigned long long canary);
void *klj_guest_thread_pointer(void);
/* Storage belongs to the retained physical thread, including late guest
 * destructors. The owner invokes retirement only after that thread is dead. */
void *klj_executable_thread_tls(void);
int klj_executable_signal(int signal, int code, void *ucontext);
int klj_executable_signal_enable(void);
void klj_executable_signal_invoke(void *handler, int signal, const void *linux_info, void *ucontext);
void klj_executable_signal_chain(void *handler, int flags, int signal,
                                 const void *linux_info, void *linux_context);
int klj_executable_signal_action(int signal,const void *action,void *previous);
int klj_executable_protect(void *address, size_t length, int protection);
void klj_executable_code_forget(void *address,size_t length);
void *klj_executable_map(void *address, size_t length, int protection, int native_flags, int descriptor, long long offset);
#ifdef __cplusplus
}
#endif
