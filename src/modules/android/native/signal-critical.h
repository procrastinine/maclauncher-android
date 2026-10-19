#pragma once
#include <errno.h>
#include <pthread.h>
#include <signal.h>

/* Native metadata must not retain its locks across an asynchronous guest
 * handler. The kernel retains pending signals and their ordering; restoring
 * the caller's exact mask delivers them after the native critical section.
 * Faults keep their original execution-domain dispatcher. Never span a wait
 * or guest callback with this guard. */
typedef sigset_t KljSignalCritical;
static inline void klj_signal_critical_enter(KljSignalCritical *previous){
    int saved=errno;sigset_t asynchronous;sigfillset(&asynchronous);
    sigdelset(&asynchronous,SIGSEGV);sigdelset(&asynchronous,SIGBUS);
    sigdelset(&asynchronous,SIGILL);sigdelset(&asynchronous,SIGFPE);
    sigdelset(&asynchronous,SIGTRAP);sigdelset(&asynchronous,SIGABRT);
    sigdelset(&asynchronous,SIGSYS);
    pthread_sigmask(SIG_BLOCK,&asynchronous,previous);errno=saved;
}
static inline void klj_signal_critical_leave(const KljSignalCritical *previous){
    int saved=errno;pthread_sigmask(SIG_SETMASK,previous,NULL);errno=saved;
}
