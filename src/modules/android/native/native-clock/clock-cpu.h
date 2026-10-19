#pragma once
#include <libproc.h>
#include <sys/resource.h>
#include "../thread-identity.h"
#include "../process-ids.h"
#include <mach/thread_info.h>
#include <mach/mach_time.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

/* Bionic encodes a logical PID/TID above CPUCLOCK_WHICH and PERTHREAD.
 * The existing identity owners retain every physical sampling target. */
static inline int klj_clock_cpu_read(int clock,struct timespec *out,int resolution){
    unsigned which=(unsigned)clock&3u;
    if(clock>=0||which==3){errno=EINVAL;return -1;}
    int id=(int)(~(uint32_t)clock>>3);uint64_t user=0,system=0;
    if(clock&4){
        thread_t thread=klj_thread_acquire(id);
        if(!thread){errno=EINVAL;return -1;}
        thread_extended_info_data_t info;mach_msg_type_number_t count=THREAD_EXTENDED_INFO_COUNT;
        kern_return_t status=thread_info(thread,THREAD_EXTENDED_INFO,(thread_info_t)&info,&count);
        mach_port_deallocate(mach_task_self(),thread);
        if(status!=KERN_SUCCESS){errno=EINVAL;return -1;}
        user=info.pth_user_time;system=info.pth_system_time;
    }else{
        // Linux gettime accepts the calling worker's TID as its process,
        // whereas getres (used by clock_getcpuclockid) requires a leader.
        if(!resolution&&id&&id==klj_thread_id())id=0;
        if(klj_process_cpu_time(id,&user,&system)){if(errno==ESRCH)errno=EINVAL;return -1;}
    }
    if(resolution){
        struct timespec discarded;
        return clock_getres(clock&4?CLOCK_THREAD_CPUTIME_ID:CLOCK_PROCESS_CPUTIME_ID,out?out:&discarded);
    }
    if(!out){errno=EFAULT;return -1;}
    uint64_t value=which==1?user:user+system;
    *out=(struct timespec){(time_t)(value/1000000000ull),(long)(value%1000000000ull)};
    return 0;
}
