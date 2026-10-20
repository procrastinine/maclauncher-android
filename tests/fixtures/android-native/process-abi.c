#include "native-process-imports.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv){
 assert(argc==2&&klj_process_ids_configure(argv[1])>0);
 assert(klj_thread_id()==klj_process_id());
 alarm(15);
 assert(kl_errno_to_linux(EOWNERDEAD)==130);
 assert(kl_errno_to_linux(ENOTRECOVERABLE)==131);
 assert(kl_errno_to_linux(EBADMSG)==74);
 assert(kl_errno_to_linux(ENODATA)==61);
 assert(kl_errno_to_linux(ENOLINK)==67);
 assert(kl_errno_to_linux(ETIME)==62);
 assert(kl_errno_from_linux(130)==EOWNERDEAD);
 assert(kl_errno_from_linux(110)==ETIMEDOUT);
 siginfo_t native_signal={0};native_signal.si_signo=SIGCHLD;native_signal.si_errno=EOWNERDEAD;native_signal.si_code=CLD_KILLED;native_signal.si_status=SIGUSR1;native_signal.si_uid=getuid();
 klj_signal_info translated_signal;klj_signal_info_to_guest(&native_signal,&translated_signal);
 assert(translated_signal.signal==17&&translated_signal.error==130&&translated_signal.data.sender.status==10&&translated_signal.data.sender.uid==klj_process_uid());
 assert(!strcmp(klj_process_strerror(110),strerror(ETIMEDOUT)));
 char message[256];assert(klj_process_strerror_r(111,message,sizeof(message))==0&&!strcmp(message,strerror(ECONNREFUSED)));
 assert(klj_process_abi_lookup("waitid")&&klj_process_abi_lookup("getrlimit64"));
 assert(klj_process_abi_lookup("ptrace"));
 errno=0;assert(klj_process_ptrace(16,klj_process_id(),NULL,NULL)==-1&&errno==ESRCH);
 errno=0;assert(klj_process_ptrace(16,0,NULL,NULL)==-1&&errno==EPERM);
 // TRACEME can succeed or be refused by the process sandbox, but the adapter
 // must call the real kernel and preserve its result. It never attaches to a
 // foreign process or requests debugger privileges.
 pid_t trace_child=fork();assert(trace_child>=0);
 if(!trace_child){errno=0;long result=klj_process_ptrace(0,0,NULL,NULL);_exit(result==0?0:(errno==EPERM?1:2));}
 int trace_status;assert(waitpid(trace_child,&trace_status,0)==trace_child&&WIFEXITED(trace_status)&&WEXITSTATUS(trace_status)<=1);
 struct rlimit native,before,after;klj_process_limit guest;
 assert(getrlimit(RLIMIT_NOFILE,&native)==0&&getrlimit(RLIMIT_NPROC,&before)==0);
 int ceiling=0;size_t ceiling_size=sizeof(ceiling);assert(sysctlbyname("kern.maxfilesperproc",&ceiling,&ceiling_size,NULL,0)==0&&ceiling>0);
 uint64_t maximum=native.rlim_max==RLIM_INFINITY?(uint64_t)ceiling:native.rlim_max;if(maximum>(uint64_t)ceiling)maximum=(uint64_t)ceiling;
 uint64_t current=native.rlim_cur==RLIM_INFINITY?maximum:native.rlim_cur;if(current>maximum)current=maximum;
 assert(klj_process_getrlimit(7,&guest)==0&&guest.current==current&&guest.maximum==maximum);
 klj_process_limit excessive={1,(uint64_t)ceiling+1};errno=0;assert(klj_process_setrlimit(7,&excessive)==-1&&errno==EPERM);
 klj_process_limit unlimited={UINT64_MAX,UINT64_MAX};errno=0;assert(klj_process_setrlimit(7,&unlimited)==-1&&errno==EPERM);
 assert(klj_process_setrlimit(7,&guest)==0&&getrlimit(RLIMIT_NPROC,&after)==0);
 assert(before.rlim_cur==after.rlim_cur&&before.rlim_max==after.rlim_max);
 klj_process_limit lower={64,maximum};assert(klj_process_setrlimit(7,&lower)==0&&klj_process_getrlimit(7,&guest)==0&&guest.current==64&&guest.maximum==maximum);
 lower.current=maximum;assert(klj_process_setrlimit(7,&lower)==0);
 assert(getrlimit(RLIMIT_FSIZE,&native)==0&&klj_process_getrlimit(1,&guest)==0);
 assert(guest.current==(native.rlim_cur==RLIM_INFINITY?UINT64_MAX:native.rlim_cur));
 struct rusage usage={0};usage.ru_utime.tv_sec=7;usage.ru_utime.tv_usec=999999;usage.ru_stime.tv_sec=9;usage.ru_stime.tv_usec=123;usage.ru_maxrss=123*1024;usage.ru_minflt=35;
 struct {klj_process_rusage value;uint64_t guard;} output;memset(&output,0xa5,sizeof(output));uint64_t guard=output.guard;
 klj_process_usage(&usage,&output.value);assert(output.value.user.sec==7&&output.value.user.usec==999999&&output.value.system.usec==123&&output.value.maxrss==123&&output.value.minflt==35&&output.guard==guard);
 assert(klj_process_getrusage(1,&output.value)==0&&output.value.user.sec>=0&&output.guard==guard);
 int pipefd[2];assert(pipe(pipefd)==0);pid_t child=klj_process_fork();assert(child>=0);
 if(!child){close(pipefd[0]);if(klj_process_setpgrp()||getpgrp()!=getpid())_exit(9);char ready='r';write(pipefd[1],&ready,1);for(;;)pause();}
 close(pipefd[1]);char ready=0;assert(read(pipefd[0],&ready,1)==1&&ready=='r');close(pipefd[0]);
 assert(klj_process_signal(child,SIGSTOP)==0);int status=0;
 assert(klj_process_waitpid(child,&status,2)==child&&status==((19<<8)|0x7f));
 assert(klj_process_signal(child,SIGCONT)==0);assert(klj_process_waitpid(child,&status,8)==child&&status==0xffff);
 assert(klj_process_killpg(child,10)==0);assert(klj_process_wait4(child,&status,0,&output.value)==child&&(status&0x7f)==10&&output.guard==guard);
 child=klj_process_fork();assert(child>=0);if(!child)_exit(73);
 klj_signal_info info;memset(&info,0xa5,sizeof(info));
 assert(klj_process_waitid(1,(unsigned)child,&info,4|0x01000000)==0);
 assert(info.signal==17&&info.data.sender.pid==child&&info.data.sender.status==73);
 assert(klj_process_waitpid(child,&status,0)==child&&((status>>8)&255)==73);
 puts("PASS original errno identities, rlimit selection, rusage layout/units, process groups and Linux child statuses");
}
