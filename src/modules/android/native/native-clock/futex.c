/* Darwin wait ownership for Linux futex callers. Guest words remain authoritative;
 * each waiter parks on its own kernel word, without retaining native locks. */
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "../signal-critical.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "clock-domain.h"
#include "futex-diagnostics.h"

enum { KLJ_WAIT=0,KLJ_WAKE=1,KLJ_REQUEUE=3,KLJ_CMP_REQUEUE=4,KLJ_WAKE_OP=5,
       KLJ_WAIT_BITSET=9,KLJ_WAKE_BITSET=10,KLJ_PRIVATE=128,KLJ_REALTIME=256 };
typedef struct klj_futex_waiter {
    struct klj_futex_waiter *next,*wake_next;
    int32_t *address;
    uint32_t mask;
    _Atomic uint32_t references,woken;
    bool linked;
    KljFutexWaitState observation;
} klj_futex_waiter;
/* Buckets index live waiters only; there is no limit on addresses or waiters.
 * One short metadata lock also makes two-address operations and requeue atomic.
 * It is never held while sleeping or running guest code. */
static pthread_mutex_t klj_futex_lock=PTHREAD_MUTEX_INITIALIZER;
static klj_futex_waiter *klj_futex_waiters[256];
int klj_futex_snapshot(KljFutexWaitState *states,uint32_t capacity,uint32_t *count,uint32_t *more){
    if(!states||!capacity||capacity>128||!count||!more)return EINVAL;
    *count=0;*more=0;
    KljSignalCritical signals;klj_signal_critical_enter(&signals);
    int error=pthread_mutex_trylock(&klj_futex_lock);
    if(!error){
        for(unsigned slot=0;slot<256&&!*more;slot++)for(klj_futex_waiter *waiter=klj_futex_waiters[slot];waiter;waiter=waiter->next){
            if(*count==capacity){*more=1;break;}
            KljFutexWaitState state=waiter->observation;
            state.address=(uintptr_t)waiter->address;state.parker=(uintptr_t)&waiter->woken;
            states[(*count)++]=state;
        }
        pthread_mutex_unlock(&klj_futex_lock);
    }
    klj_signal_critical_leave(&signals);return error;
}
static unsigned klj_futex_hash(const void *address){return (unsigned)(((uintptr_t)address>>2)*UINT64_C(0x9e3779b97f4a7c15)>>56);}
static void klj_futex_insert(klj_futex_waiter *waiter){unsigned slot=klj_futex_hash(waiter->address);waiter->next=klj_futex_waiters[slot];klj_futex_waiters[slot]=waiter;waiter->linked=true;}
static void klj_futex_remove(klj_futex_waiter *waiter){
    if(!waiter->linked)return;
    klj_futex_waiter **entry=&klj_futex_waiters[klj_futex_hash(waiter->address)];
    while(*entry&&*entry!=waiter)entry=&(*entry)->next;
    if(*entry)*entry=waiter->next;
    waiter->linked=false;
}
static void klj_futex_release(klj_futex_waiter *waiter){
    KljSignalCritical signals;klj_signal_critical_enter(&signals);
    if(atomic_fetch_sub_explicit(&waiter->references,1,memory_order_acq_rel)==1)free(waiter);
    klj_signal_critical_leave(&signals);
}
static void klj_futex_cancel(void *owner){
    klj_futex_waiter *waiter=owner;KljSignalCritical signals;klj_signal_critical_enter(&signals);
    pthread_mutex_lock(&klj_futex_lock);klj_futex_remove(waiter);pthread_mutex_unlock(&klj_futex_lock);
    klj_futex_release(waiter);klj_signal_critical_leave(&signals);
}
static long klj_futex_wake(int32_t *address,uint32_t count,uint32_t mask,klj_futex_waiter **wakes){
    long woken=0;
    for(klj_futex_waiter *waiter=klj_futex_waiters[klj_futex_hash(address)],*next;waiter&&woken<count;waiter=next){
        next=waiter->next;
        if(waiter->address!=address||!(waiter->mask&mask))continue;
        // Retain before publication: the waiter may leave as soon as it sees
        // woken, while the kernel wake runs after this metadata lock is gone.
        atomic_fetch_add_explicit(&waiter->references,1,memory_order_relaxed);
        klj_futex_remove(waiter);waiter->wake_next=*wakes;*wakes=waiter;
        atomic_store_explicit(&waiter->woken,1,memory_order_release);++woken;
    }
    return woken;
}
static struct timespec klj_futex_deadline(struct timespec relative,int realtime){
    struct timespec now;klj_clock_read(realtime?0:1,&now);
    __int128 seconds=(__int128)now.tv_sec+relative.tv_sec;
    long nanos=now.tv_nsec+relative.tv_nsec;if(nanos>=1000000000L){nanos-=1000000000L;++seconds;}
    if(seconds>INT64_MAX)return (struct timespec){INT64_MAX,999999999};
    return (struct timespec){(time_t)seconds,nanos};
}
/* Non-private Linux futexes on shared mappings are keyed by the kernel's
 * backing object and offset, not this process's virtual address. Ask the same
 * Mach mapping owner; do not create another mapping or waiter registry. */
static int klj_futex_shared(const int32_t *word){
    mach_vm_address_t address=(mach_vm_address_t)(uintptr_t)word,start=address;
    mach_vm_size_t size=0;mach_port_t object=MACH_PORT_NULL;
    vm_region_basic_info_data_64_t info={0};mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
    kern_return_t result=mach_vm_region(mach_task_self(),&start,&size,VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info,&count,&object);
    if(object!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),object);
    if(result!=KERN_SUCCESS||start>address||size<sizeof *word||address-start>size-sizeof *word||!(info.protection&VM_PROT_READ)){
        errno=EFAULT;return -1;
    }
    return info.inheritance==VM_INHERIT_SHARE;
}
/* Darwin sys/ulock.h kernel ABI. NO_ERRNO leaves the caller's errno intact;
 * Linux's syscall wrapper performs the existing host-to-guest conversion. */
enum { KLJ_UL_PRIVATE=1,KLJ_UL_SHARED=3,KLJ_UL_CANCEL=0x00020000,KLJ_UL_NO_ERRNO=0x01000000 };
extern int __ulock_wait2(uint32_t,void*,uint64_t,uint64_t,uint64_t);
extern int __ulock_wake(uint32_t,void*,uint64_t);
/* AIM's parker uses the same Darwin compare-and-wait operation. Keep the
 * established waiter table for exact bitsets/requeue; kernel waits and wakes
 * run outside its metadata lock and outside the asynchronous-signal guard. */
static void klj_futex_dispatch(klj_futex_waiter *wakes){
    while(wakes){
        klj_futex_waiter *waiter=wakes;wakes=waiter->wake_next;
        (void)__ulock_wake(KLJ_UL_PRIVATE|KLJ_UL_NO_ERRNO,&waiter->woken,0);
        klj_futex_release(waiter);
    }
}
static long klj_futex_shared_wait(int32_t *address,uint32_t expected,const struct timespec *timeout,bool absolute,bool realtime){
    if(timeout&&(!klj_nsec_valid(timeout)||timeout->tv_sec<0)){errno=EINVAL;return -1;}
    if(__atomic_load_n(address,__ATOMIC_SEQ_CST)!=(int32_t)expected){errno=EAGAIN;return -1;}
    struct timespec deadline={0};if(timeout)deadline=absolute?*timeout:klj_futex_deadline(*timeout,realtime);
    for(;;){
        uint64_t nanos=0;
        if(timeout){
            struct timespec left=klj_clock_remaining(realtime?0:1,&deadline);
            if(!left.tv_sec&&!left.tv_nsec){errno=ETIMEDOUT;return -1;}
            __uint128_t wide=(__uint128_t)left.tv_sec*1000000000u+(unsigned long)left.tv_nsec;
            nanos=wide>UINT64_MAX?UINT64_MAX:(uint64_t)wide;
        }
        int result=__ulock_wait2(KLJ_UL_SHARED|KLJ_UL_NO_ERRNO,address,expected,nanos,0);
        if(result>=0)return 0;
        // A changed wall clock can extend an absolute real-time deadline.
        if(result==-ETIMEDOUT&&timeout)continue;
        errno=-result;return -1;
    }
}
static long klj_futex_shared_wake(int32_t *address,uint32_t count){
    long woken=0;
    // ULF_WAKE_ALL reports success, not the number woken. Individual kernel
    // wakes retain the Linux count for both bounded and INT_MAX requests.
    while((uint32_t)woken<count){
        int result=__ulock_wake(KLJ_UL_SHARED|KLJ_UL_NO_ERRNO,address,0);
        if(result==-ENOENT)break;
        if(result<0){errno=-result;return -1;}
        ++woken;
    }
    return woken;
}
static long klj_futex_wait(int32_t *address,uint32_t expected,uint32_t mask,const struct timespec *timeout,bool absolute,bool realtime,int operation){
    if(timeout&&(!klj_nsec_valid(timeout)||timeout->tv_sec<0)){errno=EINVAL;return -1;}
    struct timespec deadline={0};if(timeout)deadline=absolute?*timeout:klj_futex_deadline(*timeout,realtime);
    KljSignalCritical signals;klj_signal_critical_enter(&signals);
    klj_futex_waiter *waiter=calloc(1,sizeof *waiter);
    if(!waiter){klj_signal_critical_leave(&signals);return -1;}
    waiter->address=address;waiter->mask=mask;atomic_init(&waiter->references,1);
    waiter->observation=(KljFutexWaitState){.threadHandle=(uintptr_t)pthread_self(),.initialAddress=(uintptr_t)address,
        .operation=(uint32_t)operation,.expected=expected,.mask=mask,.hasTimeout=timeout!=NULL,.realtime=realtime,
        .deadlineSeconds=deadline.tv_sec,.deadlineNanoseconds=deadline.tv_nsec};
    pthread_mutex_lock(&klj_futex_lock);
    if(__atomic_load_n(address,__ATOMIC_SEQ_CST)!=(int32_t)expected){
        pthread_mutex_unlock(&klj_futex_lock);klj_futex_release(waiter);
        klj_signal_critical_leave(&signals);errno=EAGAIN;return -1;
    }
    klj_futex_insert(waiter);pthread_mutex_unlock(&klj_futex_lock);
    int status=0;
    pthread_cleanup_push(klj_futex_cancel,waiter);
    klj_signal_critical_leave(&signals);
    while(!atomic_load_explicit(&waiter->woken,memory_order_acquire)){
        uint64_t nanos=0;
        if(timeout){
            struct timespec left=klj_clock_remaining(realtime?0:1,&deadline);
            if(!left.tv_sec&&!left.tv_nsec){status=ETIMEDOUT;break;}
            __uint128_t wide=(__uint128_t)left.tv_sec*1000000000u+(unsigned long)left.tv_nsec;
            nanos=wide>UINT64_MAX?UINT64_MAX:(uint64_t)wide;
        }
        int result=__ulock_wait2(KLJ_UL_PRIVATE|KLJ_UL_NO_ERRNO|KLJ_UL_CANCEL,&waiter->woken,0,nanos,0);
        pthread_testcancel();
        if(result<0&&result!=-EINTR&&result!=-ETIMEDOUT){status=-result;break;}
    }
    klj_signal_critical_enter(&signals);pthread_mutex_lock(&klj_futex_lock);
    if(atomic_load_explicit(&waiter->woken,memory_order_acquire))status=0;
    klj_futex_remove(waiter);pthread_mutex_unlock(&klj_futex_lock);
    pthread_cleanup_pop(0);
    klj_futex_release(waiter);klj_signal_critical_leave(&signals);
    if(status){errno=status;return -1;}return 0;
}
static bool klj_futex_compare(unsigned comparison,int32_t previous,int32_t argument){
    switch(comparison){case 0:return previous==argument;case 1:return previous!=argument;case 2:return previous<argument;case 3:return previous<=argument;case 4:return previous>argument;case 5:return previous>=argument;}return false;
}
long klj_futex(int32_t *address,int operation,uint32_t value,const struct timespec *timeout,int32_t *second,uint32_t value3){
    if(!address){errno=EFAULT;return -1;}if((uintptr_t)address&3){errno=EINVAL;return -1;}
    int command=operation&~(KLJ_PRIVATE|KLJ_REALTIME);bool realtime=(operation&KLJ_REALTIME)!=0;
    int shared=operation&KLJ_PRIVATE?0:klj_futex_shared(address);if(shared<0)return -1;
    if(command==KLJ_WAIT||command==KLJ_WAIT_BITSET){
        uint32_t mask=command==KLJ_WAIT?UINT32_MAX:value3;if(!mask){errno=EINVAL;return -1;}
        if(shared){
            // The kernel primitive has no selective bitset or requeue ABI.
            // Do not report success after waking unrelated waiters.
            if(mask!=UINT32_MAX){errno=ENOSYS;return -1;}
            return klj_futex_shared_wait(address,value,timeout,command==KLJ_WAIT_BITSET,realtime);
        }
        return klj_futex_wait(address,value,mask,timeout,command==KLJ_WAIT_BITSET,realtime,operation);
    }
    if(command==KLJ_WAKE||command==KLJ_WAKE_BITSET){
        uint32_t mask=command==KLJ_WAKE?UINT32_MAX:value3;if(!mask||(int32_t)value<0){errno=EINVAL;return -1;}
        if(shared){if(mask!=UINT32_MAX){errno=ENOSYS;return -1;}return klj_futex_shared_wake(address,value);}
        KljSignalCritical signals;klj_signal_critical_enter(&signals);klj_futex_waiter *wakes=NULL;
        pthread_mutex_lock(&klj_futex_lock);long count=klj_futex_wake(address,value,mask,&wakes);pthread_mutex_unlock(&klj_futex_lock);
        klj_signal_critical_leave(&signals);klj_futex_dispatch(wakes);return count;
    }
    if(command==KLJ_REQUEUE||command==KLJ_CMP_REQUEUE||command==KLJ_WAKE_OP){
        if(!second){errno=EFAULT;return -1;}if((uintptr_t)second&3){errno=EINVAL;return -1;}
        uint32_t count2=(uint32_t)(uintptr_t)timeout;
        if((int32_t)value<0||(int32_t)count2<0){errno=EINVAL;return -1;}
        if(command!=KLJ_WAKE_OP&&address==second){errno=EINVAL;return -1;}
        unsigned opcode=value3>>28,comparison=(value3>>24)&15;
        if(command==KLJ_WAKE_OP&&((opcode&7)>4||comparison>5)){errno=ENOSYS;return -1;}
        int second_shared=operation&KLJ_PRIVATE?0:klj_futex_shared(second);if(second_shared<0)return -1;
        if(command!=KLJ_WAKE_OP&&(shared||second_shared)){errno=ENOSYS;return -1;}
        KljSignalCritical signals;klj_signal_critical_enter(&signals);klj_futex_waiter *wakes=NULL;
        pthread_mutex_lock(&klj_futex_lock);
        if(command==KLJ_CMP_REQUEUE&&__atomic_load_n(address,__ATOMIC_SEQ_CST)!=(int32_t)value3){pthread_mutex_unlock(&klj_futex_lock);klj_signal_critical_leave(&signals);errno=EAGAIN;return -1;}
        long result;uint32_t shared_first=0,shared_second=0;
        if(command==KLJ_WAKE_OP){
            int32_t operand=(int32_t)(value3<<8)>>20,argument=(int32_t)(value3<<20)>>20,previous=0;
            if(opcode&8)operand=(int32_t)(1u<<((uint32_t)operand&31));
            switch(opcode&7){
                case 0:previous=__atomic_exchange_n(second,operand,__ATOMIC_SEQ_CST);break;
                case 1:previous=__atomic_fetch_add(second,operand,__ATOMIC_SEQ_CST);break;
                case 2:previous=__atomic_fetch_or(second,operand,__ATOMIC_SEQ_CST);break;
                case 3:previous=__atomic_fetch_and(second,~operand,__ATOMIC_SEQ_CST);break;
                case 4:previous=__atomic_fetch_xor(second,operand,__ATOMIC_SEQ_CST);break;
            }
            shared_first=shared?value:0;
            result=shared?0:klj_futex_wake(address,value,UINT32_MAX,&wakes);
            if(klj_futex_compare(comparison,previous,argument)){
                if(second_shared)shared_second=count2;
                else result+=klj_futex_wake(second,count2,UINT32_MAX,&wakes);
            }
        }else{
            result=klj_futex_wake(address,value,UINT32_MAX,&wakes);uint32_t moved=0;
            for(klj_futex_waiter *waiter=klj_futex_waiters[klj_futex_hash(address)],*next;waiter&&moved<count2;waiter=next){
                next=waiter->next;if(waiter->address!=address)continue;
                klj_futex_remove(waiter);waiter->address=second;klj_futex_insert(waiter);++moved;
            }
            result+=moved;
        }
        pthread_mutex_unlock(&klj_futex_lock);klj_signal_critical_leave(&signals);klj_futex_dispatch(wakes);
        if(shared_first){long count=klj_futex_shared_wake(address,shared_first);if(count<0)return count;result+=count;}
        if(shared_second){long count=klj_futex_shared_wake(second,shared_second);if(count<0)return count;result+=count;}
        return result;
    }
    errno=ENOSYS;return -1;
}
