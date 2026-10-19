#include "signal-bridge.h"
#include "process-identity.h"
#include "thread-identity.h"
#include "process-ids.h"
#include "executable-memory/api.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
extern int kl_errno_to_linux(int);
extern int kl_errno_from_linux(int);

/* Linux AArch64 UAPI numbers are not Darwin numbers. Keep this translation
 * shared by registration, delivery, masks and android.os.Process. The two
 * Linux-only standard signals use Darwin's otherwise distinct EMT/INFO slots. */
static const int signals[]={0,SIGHUP,SIGINT,SIGQUIT,SIGILL,SIGTRAP,SIGABRT,
 SIGBUS,SIGFPE,SIGKILL,SIGUSR1,SIGSEGV,SIGUSR2,SIGPIPE,SIGALRM,SIGTERM,
 SIGEMT,SIGCHLD,SIGCONT,SIGSTOP,SIGTSTP,SIGTTIN,SIGTTOU,SIGURG,SIGXCPU,
 SIGXFSZ,SIGVTALRM,SIGPROF,SIGWINCH,SIGIO,SIGINFO,SIGSYS};
int klj_signal_to_host(int signal){return signal>=0&&signal<(int)(sizeof(signals)/sizeof(*signals))?signals[signal]:-1;}
int klj_signal_to_guest(int signal){for(unsigned i=0;i<sizeof(signals)/sizeof(*signals);i++)if(signals[i]==signal)return (int)i;return -1;}
sigset_t klj_signal_mask_to_host(uint64_t mask){sigset_t result=0;for(int i=1;i<32;i++)if(mask&(UINT64_C(1)<<(i-1)))sigaddset(&result,signals[i]);return result;}
uint64_t klj_signal_mask_to_guest(sigset_t mask){uint64_t result=0;for(int i=1;i<32;i++)if(sigismember(&mask,signals[i])==1)result|=UINT64_C(1)<<(i-1);return result;}
_Static_assert(sizeof(klj_signal_info)==128,"Android ARM64 siginfo_t");
int klj_signal_is_hardware_fault(int signal,const siginfo_t *info){
 // Darwin's software SI_USER/SI_QUEUE/... codes are positive (0x10001+).
 // Only the matching signal's documented exception codes establish a CPU
 // fault. A kill/raise can interrupt native libc while targeting a guest
 // handler, so the interrupted PC alone cannot choose its execution domain.
 if(!info||info->si_signo!=signal)return 0;
 int code=info->si_code;
 switch(signal){
  case SIGILL:return code==ILL_ILLOPC||code==ILL_ILLTRP||code==ILL_PRVOPC||code==ILL_ILLOPN||code==ILL_ILLADR||code==ILL_PRVREG||code==ILL_COPROC||code==ILL_BADSTK;
  case SIGFPE:return code==FPE_FLTDIV||code==FPE_FLTOVF||code==FPE_FLTUND||code==FPE_FLTRES||code==FPE_FLTINV||code==FPE_FLTSUB||code==FPE_INTDIV||code==FPE_INTOVF;
  case SIGSEGV:return code==SEGV_MAPERR||code==SEGV_ACCERR;
  case SIGBUS:return code==BUS_ADRALN||code==BUS_ADRERR||code==BUS_OBJERR;
  case SIGTRAP:return code==TRAP_BRKPT||code==TRAP_TRACE;
  default:return 0;
 }
}
void klj_signal_info_to_guest(const siginfo_t *native,klj_signal_info *guest){
 memset(guest,0,sizeof(*guest));if(!native)return;
 guest->signal=klj_signal_to_guest(native->si_signo);guest->error=kl_errno_to_linux(native->si_errno);guest->code=native->si_code;
 switch(native->si_code){case SI_USER:guest->code=0;break;case SI_QUEUE:guest->code=-1;break;case SI_TIMER:guest->code=-2;break;case SI_ASYNCIO:guest->code=-4;break;case SI_MESGQ:guest->code=-3;break;}
 if(guest->code<=0||native->si_signo==SIGCHLD){
  guest->data.sender.pid=native->si_pid;guest->data.sender.uid=klj_process_uid_from_host(native->si_uid);guest->data.sender.value=(uintptr_t)native->si_value.sival_ptr;
  if(native->si_signo==SIGCHLD){
   guest->data.sender.status=native->si_status;
   if(native->si_code==CLD_KILLED||native->si_code==CLD_DUMPED||native->si_code==CLD_STOPPED||native->si_code==CLD_CONTINUED||native->si_code==CLD_TRAPPED)guest->data.sender.status=klj_signal_to_guest(native->si_status);
  }
 }
 else if(native->si_signo==SIGIO){guest->data.poll.band=native->si_band;}
 else guest->data.fault.address=native->si_addr;
}
int klj_signal_mask(int how,const uint64_t *mask,uint64_t *previous){
 if(mask&&(how<0||how>2))return EINVAL;
 sigset_t in=mask?klj_signal_mask_to_host(*mask):0,out=0;
 int result=pthread_sigmask(how+1,mask?&in:NULL,previous?&out:NULL);
 if(!result&&previous)*previous=klj_signal_mask_to_guest(out);
 return result;
}
int klj_signal_thread(pthread_t thread,int signal){int host=klj_signal_to_host(signal);return host<0?EINVAL:pthread_kill(thread,host);}
extern int __pthread_kill(mach_port_t,int);
int klj_signal_thread_id(int process,int id,int signal){
 int host=klj_signal_to_host(signal);
 if(process<=0||id<=0||host<0){errno=EINVAL;return -1;}
 if(process!=klj_process_id()){errno=ESRCH;return -1;}
 thread_t thread=klj_thread_acquire(id);if(!thread)return -1;
 // The retained Mach right cannot be reused for a newly created thread.
 int result=__pthread_kill(thread,host),saved=errno;
 mach_port_deallocate(mach_task_self(),thread);errno=saved;return result;
}
static int guest_tkill(int thread,int signal){return klj_signal_thread_id(klj_process_id(),thread,signal);}
static int guest_kill(pid_t pid,int signal){int host=klj_signal_to_host(signal);if(host<0){errno=EINVAL;return -1;}return klj_process_signal(pid,host);}
static int guest_raise(int signal){int result=klj_signal_thread(pthread_self(),signal);if(result){errno=result;return -1;}return 0;}
static int guest_empty(uint64_t *mask){if(!mask){errno=EINVAL;return -1;}*mask=0;return 0;}
static int guest_fill(uint64_t *mask){if(!mask){errno=EINVAL;return -1;}*mask=UINT64_MAX;return 0;}
static int valid_set(uint64_t *mask,int signal){if(!mask||signal<1||signal>64){errno=EINVAL;return 0;}return 1;}
static int guest_add(uint64_t *mask,int signal){if(!valid_set(mask,signal))return -1;*mask|=UINT64_C(1)<<(signal-1);return 0;}
static int guest_delete(uint64_t *mask,int signal){if(!valid_set(mask,signal))return -1;*mask&=~(UINT64_C(1)<<(signal-1));return 0;}
static int guest_member(const uint64_t *mask,int signal){if(!valid_set((uint64_t *)mask,signal))return -1;return !!(*mask&(UINT64_C(1)<<(signal-1)));}
static int guest_pending(uint64_t *mask){sigset_t host=0;int result=sigpending(&host);if(!result)*mask=klj_signal_mask_to_guest(host);return result;}
static int guest_suspend(const uint64_t *mask){sigset_t host=klj_signal_mask_to_host(*mask);return sigsuspend(&host);}
static int guest_wait(const uint64_t *mask,int *signal){sigset_t host=klj_signal_mask_to_host(*mask);int value=0,result=sigwait(&host,&value);if(!result)*signal=klj_signal_to_guest(value);return kl_errno_to_linux(result);}
static int guest_thread_mask(int how,const uint64_t *mask,uint64_t *previous){return kl_errno_to_linux(klj_signal_mask(how,mask,previous));}
static int guest_thread_signal(pthread_t thread,int signal){return kl_errno_to_linux(klj_signal_thread(thread,signal));}
static int guest_sigprocmask(int how,const uint64_t *mask,uint64_t *previous){int result=klj_signal_mask(how,mask,previous);if(result){errno=result;return -1;}return 0;}
static char *guest_strsignal(int signal){int host=klj_signal_to_host(signal);return strsignal(host<0?signal:host);}
typedef struct {int32_t flags,pad;void *handler;uint64_t mask;void *restorer;} guest_action;
extern int klb_sigaction(int,const guest_action *,guest_action *);
// Original arm64 __kernel_sigaction orders handler/unsigned-long flags/
// restorer/mask. Bionic's public LP64 sigaction orders its int flags first.
// Raw SVC and libc callers converge on the same registration/chaining owner.
typedef struct {void *handler;uint64_t flags;void *restorer;uint64_t mask;} kernel_action;
static int guest_kernel_sigaction(int signal,const kernel_action *action,kernel_action *previous,size_t mask_size){
 if(mask_size!=sizeof(uint64_t)){errno=EINVAL;return -1;}
 if(signal>=32&&signal<=64){
  int64_t result=klj_guest_signal_action(signal,(const KljGuestSignalAction *)action,(KljGuestSignalAction *)previous);
  if(result<0){errno=kl_errno_from_linux((int)-result);return -1;}return 0;
 }
 guest_action in={0},out={0};
 if(action){in.handler=action->handler;in.flags=(int32_t)action->flags;in.mask=action->mask;in.restorer=action->restorer;}
 int result=klb_sigaction(signal,action?&in:NULL,previous?&out:NULL);
 if(!result&&previous)*previous=(kernel_action){.handler=out.handler,.flags=(uint32_t)out.flags,.restorer=out.restorer,.mask=out.mask};
 return result;
}
static void *guest_signal(int signal,void *handler){guest_action in={.flags=0x10000000,.handler=handler},out={0};return klb_sigaction(signal,&in,&out)?(void *)SIG_ERR:out.handler;}
void *klj_signal_lookup(const char *name){
#define BIND(n,f) if(!strcmp(name,n))return (void *)(f)
 BIND("kill",guest_kill);BIND("raise",guest_raise);BIND("signal",guest_signal);BIND("bsd_signal",guest_signal);
 BIND("tgkill",klj_signal_thread_id);BIND("tkill",guest_tkill);
 BIND("sigaction64",klb_sigaction);
 BIND("__rt_sigaction",guest_kernel_sigaction);
 BIND("sigemptyset",guest_empty);BIND("sigfillset",guest_fill);BIND("sigaddset",guest_add);BIND("sigdelset",guest_delete);BIND("sigismember",guest_member);
 BIND("sigpending",guest_pending);BIND("sigsuspend",guest_suspend);BIND("sigwait",guest_wait);BIND("sigprocmask",guest_sigprocmask);BIND("pthread_sigmask",guest_thread_mask);BIND("pthread_kill",guest_thread_signal);BIND("strsignal",guest_strsignal);
 BIND("sigemptyset64",guest_empty);BIND("sigfillset64",guest_fill);BIND("sigaddset64",guest_add);BIND("sigdelset64",guest_delete);BIND("sigismember64",guest_member);
 BIND("sigpending64",guest_pending);BIND("sigsuspend64",guest_suspend);BIND("sigwait64",guest_wait);BIND("sigprocmask64",guest_sigprocmask);BIND("pthread_sigmask64",guest_thread_mask);
#undef BIND
 return NULL;
}
