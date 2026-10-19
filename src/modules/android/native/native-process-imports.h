#pragma once
#include "signal-bridge.h"
#include "process-identity.h"
#include "process-ids.h"
#include "thread-identity.h"
#include "process-memory.h"
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
extern int kl_errno_to_linux(int);
extern int kl_errno_from_linux(int);

typedef struct {int64_t sec,usec;} klj_process_timeval;
typedef struct {
 klj_process_timeval user,system;
 int64_t maxrss,ixrss,idrss,isrss,minflt,majflt,nswap,inblock,oublock,msgsnd,msgrcv,nsignals,nvcsw,nivcsw;
} klj_process_rusage;
typedef struct {uint64_t current,maximum;} klj_process_limit;
_Static_assert(sizeof(klj_process_rusage)==144,"Linux ARM64 rusage");
static void klj_process_usage(const struct rusage *native,klj_process_rusage *guest){
 guest->user=(klj_process_timeval){native->ru_utime.tv_sec,native->ru_utime.tv_usec};
 guest->system=(klj_process_timeval){native->ru_stime.tv_sec,native->ru_stime.tv_usec};
 guest->maxrss=native->ru_maxrss/1024;
#define FIELD(name) guest->name=native->ru_##name
 FIELD(ixrss);FIELD(idrss);FIELD(isrss);FIELD(minflt);FIELD(majflt);FIELD(nswap);FIELD(inblock);FIELD(oublock);FIELD(msgsnd);FIELD(msgrcv);FIELD(nsignals);FIELD(nvcsw);FIELD(nivcsw);
#undef FIELD
}
static int klj_process_getrusage(int who,klj_process_rusage *guest){
 if(!guest){errno=EFAULT;return -1;}
 if(who==1){
  thread_basic_info_data_t info;mach_msg_type_number_t size=THREAD_BASIC_INFO_COUNT;thread_t thread=mach_thread_self();
  kern_return_t result=thread_info(thread,THREAD_BASIC_INFO,(thread_info_t)&info,&size);mach_port_deallocate(mach_task_self(),thread);
  if(result!=KERN_SUCCESS){errno=ESRCH;return -1;}memset(guest,0,sizeof(*guest));
  guest->user=(klj_process_timeval){info.user_time.seconds,info.user_time.microseconds};
  guest->system=(klj_process_timeval){info.system_time.seconds,info.system_time.microseconds};return 0;
 }
 struct rusage native={0};int result=getrusage(who,&native);if(!result)klj_process_usage(&native,guest);return result;
}
static int klj_process_resource(int value){
 static const int resources[]={RLIMIT_CPU,RLIMIT_FSIZE,RLIMIT_DATA,RLIMIT_STACK,RLIMIT_CORE,RLIMIT_RSS,RLIMIT_NPROC,RLIMIT_NOFILE,RLIMIT_MEMLOCK,RLIMIT_AS};
 return value>=0&&value<(int)(sizeof(resources)/sizeof(*resources))?resources[value]:-1;
}
static int klj_process_getrlimit(int resource,klj_process_limit *limit){
 if(!limit){errno=EFAULT;return -1;}int host=klj_process_resource(resource);if(host<0){errno=EINVAL;return -1;}
 struct rlimit native;int result=getrlimit(host,&native);if(!result){limit->current=native.rlim_cur==RLIM_INFINITY?UINT64_MAX:native.rlim_cur;limit->maximum=native.rlim_max==RLIM_INFINITY?UINT64_MAX:native.rlim_max;}return result;
}
static int klj_process_setrlimit(int resource,const klj_process_limit *limit){
 if(!limit){errno=EFAULT;return -1;}int host=klj_process_resource(resource);if(host<0||limit->current>limit->maximum){errno=EINVAL;return -1;}
 struct rlimit native={limit->current>=RLIM_INFINITY?RLIM_INFINITY:limit->current,limit->maximum>=RLIM_INFINITY?RLIM_INFINITY:limit->maximum};return setrlimit(host,&native);
}
static int klj_process_status(int status){
 if(WIFCONTINUED(status))return 0xffff;
 if(WIFSTOPPED(status))return (klj_signal_to_guest(WSTOPSIG(status))<<8)|0x7f;
 if(WIFSIGNALED(status))return klj_signal_to_guest(WTERMSIG(status))|(status&0x80);
 return status;
}
static int klj_process_wait_options(unsigned options,int id){
 unsigned accepted=id?(1u|2u|4u|8u|0x01000000u|0xe0000000u):(1u|2u|8u|0xe0000000u);
 if(options&~accepted){errno=EINVAL;return -1;}
 int result=(options&1u)?WNOHANG:0;
 if(options&2u)result|=id?WSTOPPED:WUNTRACED;
 if(options&8u)result|=WCONTINUED;
 if(id&&options&4u)result|=WEXITED;
 if(id&&options&0x01000000u)result|=WNOWAIT;
 return result;
}
static pid_t klj_process_wait4(pid_t pid,int *status,int options,klj_process_rusage *usage){
 int host=klj_process_wait_options((unsigned)options,0);if(host<0)return -1;
 int result_status=0;struct rusage native={0};pid_t result=klj_process_wait_native(pid,status?&result_status:NULL,host,usage?&native:NULL);
 if(result>0){if(status)*status=klj_process_status(result_status);if(usage)klj_process_usage(&native,usage);}return result;
}
static pid_t klj_process_waitpid(pid_t pid,int *status,int options){return klj_process_wait4(pid,status,options,NULL);}
static pid_t klj_process_wait(int *status){return klj_process_wait4(-1,status,0,NULL);}
static int klj_process_waitid(int type,unsigned id,klj_signal_info *info,int options){
 int host=klj_process_wait_options((unsigned)options,1);if(host<0)return -1;
 if(type<0||type>2||!info){errno=!info?EFAULT:EINVAL;return -1;}
 static const idtype_t kinds[]={P_ALL,P_PID,P_PGID};
 siginfo_t native={0};int result=klj_process_waitid_native(kinds[type],id,&native,host);if(!result)klj_signal_info_to_guest(&native,info);return result;
}
static int klj_process_setpgrp(void){return klj_process_setpgid(0,0);}
static int klj_process_getpriority(int which,id_t id){
 if(which!=PRIO_PROCESS||!klj_thread_priority_target((int)id))return getpriority(which,id);
 int value=0,error=klj_thread_priority_get((int)id,&value);
 if(error){errno=error;return -1;}errno=0;return value;
}
static int klj_process_setpriority(int which,id_t id,int value){
 if(which!=PRIO_PROCESS||!klj_thread_priority_target((int)id))return setpriority(which,id,value);
 // Linux clamps nice requests; Java's Process API validates them separately.
 if(value<-20)value=-20;if(value>19)value=19;
 int error=klj_thread_priority_set((int)id,value);
 if(error){errno=error;return -1;}return 0;
}
static int klj_process_killpg(pid_t group,int signal){int host=klj_signal_to_host(signal);if(host<0){errno=EINVAL;return -1;}return klj_process_group_signal(group,host);}
static long klj_process_ptrace(int request,pid_t target,void *address,void *data){
 // TRACEME concerns this process alone and preserves the kernel's real result.
 // Other requests need an enrolled target and a tracer relationship. This
 // runtime does not grant guest code debugger authority over host processes.
 (void)address;(void)data;
 if(request==0)return ptrace(PT_TRACE_ME,0,NULL,0);
 errno=target==klj_process_id()?ESRCH:EPERM;return -1;
}
static char *klj_process_strerror(int error){int host=kl_errno_from_linux(error);return host<0?"Unknown error":strerror(host);}
static int klj_process_strerror_r(int error,char *buffer,size_t size){int host=kl_errno_from_linux(error);if(host<0){if(size)snprintf(buffer,size,"Unknown error %d",error);return 22;}return kl_errno_to_linux(strerror_r(host,buffer,size));}
static char *klj_process_gnu_strerror_r(int error,char *buffer,size_t size){if(klj_process_strerror_r(error,buffer,size))return klj_process_strerror(error);return buffer;}
typedef struct {int priority;} klj_process_sched_param;
static int klj_process_sched_result(int error){if(!error)return 0;errno=error;return -1;}
static int klj_process_sched_setscheduler(int target,int policy,const klj_process_sched_param *parameter){return klj_process_sched_result(parameter?klj_thread_scheduler_set(target,policy,parameter->priority):EFAULT);}
static int klj_process_sched_getscheduler(int target){int policy=0,priority=0;int error=klj_thread_scheduler_get(target,&policy,&priority,NULL);return error?klj_process_sched_result(error):policy;}
static int klj_process_sched_setparam(int target,const klj_process_sched_param *parameter){if(!parameter)return klj_process_sched_result(EFAULT);int policy=0,priority=0;int error=klj_thread_scheduler_get(target,&policy,&priority,NULL);return klj_process_sched_result(error?error:klj_thread_scheduler_set(target,policy,parameter->priority));}
static int klj_process_sched_getparam(int target,klj_process_sched_param *parameter){if(!parameter)return klj_process_sched_result(EFAULT);int policy=0;return klj_process_sched_result(klj_thread_scheduler_get(target,&policy,&parameter->priority,NULL));}
static int klj_process_sched_priority_min(int policy){if(policy==1||policy==2)return 1;if(policy==0||policy==3||policy==5||policy==6)return 0;return klj_process_sched_result(EINVAL);}
static int klj_process_sched_priority_max(int policy){int min=klj_process_sched_priority_min(policy);return min==1?99:min;}
static int klj_process_sched_rr_interval(int target,struct timespec *interval){if(!interval)return klj_process_sched_result(EFAULT);int policy=0,priority=0;long nanos=0;int error=klj_thread_scheduler_get(target,&policy,&priority,&nanos);if(!error)*interval=(struct timespec){nanos/1000000000L,nanos%1000000000L};return klj_process_sched_result(error);}
static void *klj_process_abi_lookup(const char *name){
#define BIND(n,f) if(!strcmp(name,n))return (void *)(f)
 BIND("getrusage",klj_process_getrusage);BIND("getrlimit",klj_process_getrlimit);BIND("getrlimit64",klj_process_getrlimit);BIND("setrlimit",klj_process_setrlimit);BIND("setrlimit64",klj_process_setrlimit);
 BIND("wait",klj_process_wait);BIND("waitpid",klj_process_waitpid);BIND("wait4",klj_process_wait4);BIND("waitid",klj_process_waitid);
 BIND("ptrace",klj_process_ptrace);
 BIND("process_vm_readv",klj_process_vm_readv);BIND("process_vm_writev",klj_process_vm_writev);
 BIND("sched_setscheduler",klj_process_sched_setscheduler);BIND("sched_getscheduler",klj_process_sched_getscheduler);BIND("sched_setparam",klj_process_sched_setparam);BIND("sched_getparam",klj_process_sched_getparam);
 BIND("sched_get_priority_min",klj_process_sched_priority_min);BIND("sched_get_priority_max",klj_process_sched_priority_max);BIND("sched_rr_get_interval",klj_process_sched_rr_interval);BIND("sched_yield",sched_yield);
 BIND("getpriority",klj_process_getpriority);BIND("setpriority",klj_process_setpriority);
 BIND("setpgrp",klj_process_setpgrp);BIND("killpg",klj_process_killpg);BIND("strerror",klj_process_strerror);BIND("strerror_r",klj_process_strerror_r);BIND("__gnu_strerror_r",klj_process_gnu_strerror_r);
#undef BIND
 return NULL;
}
