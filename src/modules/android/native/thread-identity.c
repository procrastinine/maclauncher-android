#include "thread-identity.h"
#include "process-ids.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>

/* Original LP32 Bionic stores the owner TID in sixteen bits. The shared kernel
 * leases allocate processes and threads in that namespace on both backends.
 * Mach rights remain private physical scheduling targets; current queries use
 * TLS and retirement releases the same lease used by the guest scheduler. */
typedef struct identity {
    struct identity *next;
    int id;
    thread_t port;
    void *memory_context;
    void (*retire_memory)(void *);
    int schedule_valid,guest_policy,guest_priority,host_policy,host_priority;
} identity;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int key_error;
static identity *identities;
static _Thread_local identity *current;
static _Thread_local void *fork_memory;
static _Thread_local void (*fork_retire_memory)(void *);

static void reap_locked(void){
    /* A pthread-key callback is not physical retirement: another destructor
     * can still enter guest code afterward. Keep the exact lease through all
     * TLS/JNI cleanup and release it only after this retained Mach thread has
     * become a dead kernel object. The same owner operations collect it. */
    for(identity **p=&identities;*p;){
        identity *entry=*p;mach_port_type_t type=0;
        kern_return_t result=mach_port_type(mach_task_self(),entry->port,&type);
        if(result==KERN_SUCCESS&&!(type&MACH_PORT_TYPE_DEAD_NAME)){p=&entry->next;continue;}
        *p=entry->next;
        mach_port_deallocate(mach_task_self(),entry->port);
        klj_thread_id_release(entry->id);
        if(entry->retire_memory)entry->retire_memory(entry->memory_context);
        free(entry);
    }
}
static void before_fork(void){
    pthread_mutex_lock(&lock);
    if(!current){
        thread_t thread=mach_thread_self();
        for(identity *entry=identities;entry;entry=entry->next)if(entry->port==thread){current=entry;break;}
        mach_port_deallocate(mach_task_self(),thread);
    }
}
static void after_fork_parent(void){pthread_mutex_unlock(&lock);}
static void after_fork_child(void){
    /* Other threads and their rights do not survive fork. Do not operate on
     * parent task port names in the child task. Re-register lazily there. */
    int reset=current&&current->schedule_valid&&(current->guest_policy&0x40000000);
    int prior_policy=current?current->guest_policy&~0x40000000:0;
    /* The calling thread survives with the same Android TP, native callback
     * cursor and ELF TLS values. Only its guest scheduling identity changes.
     * Sibling contexts retire because those physical threads do not survive. */
    if(current){fork_memory=current->memory_context;fork_retire_memory=current->retire_memory;}
    identity *entry=identities;
    while(entry){identity *next=entry->next;if(entry!=current&&entry->retire_memory)entry->retire_memory(entry->memory_context);free(entry);entry=next;}
    identities=NULL;current=NULL;
    pthread_mutex_unlock(&lock);
    if(reset){
        thread_t thread=mach_thread_self();
        if(prior_policy==1||prior_policy==2){policy_timeshare_base_data_t policy={.base_priority=31};thread_policy(thread,POLICY_TIMESHARE,(policy_base_t)&policy,POLICY_TIMESHARE_BASE_COUNT,TRUE);}
        thread_precedence_policy_data_t priority;mach_msg_type_number_t size=THREAD_PRECEDENCE_POLICY_COUNT;boolean_t defaults=FALSE;
        if(thread_policy_get(thread,THREAD_PRECEDENCE_POLICY,(thread_policy_t)&priority,&size,&defaults)==KERN_SUCCESS&&priority.importance>0){priority.importance=0;thread_policy_set(thread,THREAD_PRECEDENCE_POLICY,(thread_policy_t)&priority,THREAD_PRECEDENCE_POLICY_COUNT);}
        mach_port_deallocate(mach_task_self(),thread);
    }
}
static void initialize(void){
    key_error=pthread_atfork(before_fork,after_fork_parent,after_fork_child);
}
int klj_thread_id_adopt(int id){
    if(current){if(current->id==id)return id;errno=EALREADY;return -1;}
    if(id<KLJ_PROCESS_ID_MIN||id>KLJ_PROCESS_ID_MAX){errno=EINVAL;return -1;}
    pid_t host;int kind,process;
    if(klj_process_id_query(id,&host,&kind,&process))return -1;
    if(host!=getpid()||process!=klj_process_id()||(kind!=2&&id!=process)){errno=EPERM;return -1;}
    pthread_once(&once,initialize);
    if(key_error){errno=key_error;return -1;}
    identity *entry=calloc(1,sizeof(*entry));
    if(!entry)return -1;
    entry->port=mach_thread_self();
    entry->id=id;
    pthread_mutex_lock(&lock);
    reap_locked();
    int code=0;
    for(identity *p=identities;p;p=p->next)if(p->id==id){code=EALREADY;break;}
    if(!code){
        entry->memory_context=fork_memory;entry->retire_memory=fork_retire_memory;
        fork_memory=NULL;fork_retire_memory=NULL;
        entry->next=identities;identities=entry;current=entry;
    }
    pthread_mutex_unlock(&lock);
    if(code){mach_port_deallocate(mach_task_self(),entry->port);free(entry);errno=code;return -1;}
    return entry->id;
}
int klj_thread_id(void){
    if(current)return current->id;
    /* Darwin can clear TLV storage before a later POSIX destructor enters
     * guest code. Recover this still-live Mach thread's existing identity,
     * rather than allocating a different TID during the same lifetime. */
    thread_t thread=mach_thread_self();
    pthread_mutex_lock(&lock);reap_locked();
    for(identity *entry=identities;entry;entry=entry->next)if(entry->port==thread){current=entry;break;}
    pthread_mutex_unlock(&lock);mach_port_deallocate(mach_task_self(),thread);
    if(current)return current->id;
    int id=pthread_main_np()?klj_process_id():klj_thread_id_reserve();
    if(id<0)return -1;
    int result=klj_thread_id_adopt(id);
    if(result<0){int error=errno;klj_thread_id_release(id);errno=error;}
    return result;
}
int klj_thread_id_reserve(void){
    pthread_mutex_lock(&lock);reap_locked();pthread_mutex_unlock(&lock);
    return klj_thread_id_allocate();
}
void *klj_thread_memory_context(void){return current?current->memory_context:NULL;}
int klj_thread_memory_adopt(void *context,void (*retire)(void *)){
    if(!current&&klj_thread_id()<0)return -1;
    if(current->memory_context){errno=EALREADY;return -1;}
    current->memory_context=context;current->retire_memory=retire;return 0;
}
thread_t klj_thread_acquire(int id){
    if(id==0)return mach_thread_self();
    if(id<0){errno=ESRCH;return MACH_PORT_NULL;}
    pthread_mutex_lock(&lock);
    reap_locked();
    thread_t result=MACH_PORT_NULL;
    for(identity *p=identities;p;p=p->next)if(p->id==id){
        if(mach_port_mod_refs(mach_task_self(),p->port,MACH_PORT_RIGHT_SEND,1)==KERN_SUCCESS)result=p->port;
        break;
    }
    pthread_mutex_unlock(&lock);
    if(!result)errno=ESRCH;
    return result;
}

int klj_thread_priority_target(int id){
    if(id==0)return 1;
    pthread_mutex_lock(&lock);reap_locked();int found=0;
    for(identity *p=identities;p;p=p->next)if(p->id==id){found=1;break;}
    pthread_mutex_unlock(&lock);return found;
}
static int scheduling_error(kern_return_t code){
    if(code==KERN_SUCCESS)return 0;
    return code==KERN_TERMINATED||code==KERN_INVALID_ARGUMENT||code==MACH_SEND_INVALID_DEST?ESRCH:EPERM;
}
int klj_thread_priority_set(int id,int value){
    if(value<-20||value>19)return EINVAL;
    /* Android nice controls CPU scheduling. Darwin background QoS additionally
     * throttles filesystem/network work, so it is not a nice-value mapping.
     * Use the same actual CPU precedence for self and retained foreign threads
     * instead of introducing a separate Darwin resource-throttling tier. */
    thread_t thread=klj_thread_acquire(id);if(!thread)return errno;
    thread_precedence_policy_data_t policy={.importance=-value};
    kern_return_t code=thread_policy_set(thread,THREAD_PRECEDENCE_POLICY,(thread_policy_t)&policy,THREAD_PRECEDENCE_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(),thread);return scheduling_error(code);
}
int klj_thread_priority_get(int id,int *value){
    if(!value)return EFAULT;
    thread_t thread=klj_thread_acquire(id);if(!thread)return errno;
    thread_precedence_policy_data_t policy;
    mach_msg_type_number_t size=THREAD_PRECEDENCE_POLICY_COUNT;boolean_t defaults=FALSE;
    kern_return_t code=thread_policy_get(thread,THREAD_PRECEDENCE_POLICY,(thread_policy_t)&policy,&size,&defaults);
    if(code==KERN_SUCCESS)*value=-policy.importance;
    mach_port_deallocate(mach_task_self(),thread);return scheduling_error(code);
}

/* Linux sched_* names a thread, even when its parameter is called pid. Reuse
 * the same retained Mach rights as Process and nice, never a Darwin PID cast.
 * POSIX real-time priorities use Linux 1..99 and Darwin's declared range. */
static int native_priority(int policy,int value){
    int host=policy==1?SCHED_FIFO:SCHED_RR;
    int low=sched_get_priority_min(host),high=sched_get_priority_max(host);
    return low+(value-1)*(high-low)/98;
}
static int linux_priority(int policy,int value){
    int host=policy==1?SCHED_FIFO:SCHED_RR;
    int low=sched_get_priority_min(host),high=sched_get_priority_max(host);
    if(value<low)value=low;if(value>high)value=high;
    return high>low?1+(value-low)*98/(high-low):1;
}
static int scheduler_state(thread_t thread,int *policy,int *priority,long *quantum){
    thread_basic_info_data_t info;mach_msg_type_number_t size=THREAD_BASIC_INFO_COUNT;
    kern_return_t code=thread_info(thread,THREAD_BASIC_INFO,(thread_info_t)&info,&size);
    if(code!=KERN_SUCCESS)return scheduling_error(code);
    *policy=info.policy==POLICY_FIFO?1:info.policy==POLICY_RR?2:0;*priority=0;if(quantum)*quantum=0;
    if(*policy){
        union {policy_fifo_info_data_t fifo;policy_rr_info_data_t rr;} detail={0};
        size=*policy==1?POLICY_FIFO_INFO_COUNT:POLICY_RR_INFO_COUNT;
        code=thread_info(thread,*policy==1?THREAD_SCHED_FIFO_INFO:THREAD_SCHED_RR_INFO,(thread_info_t)&detail,&size);
        if(code!=KERN_SUCCESS)return scheduling_error(code);
        *priority=detail.fifo.base_priority;
        if(quantum&&*policy==2)*quantum=(long)detail.rr.quantum*1000000L;
    }
    return 0;
}
int klj_thread_scheduler_get(int id,int *policy,int *priority,long *quantum){
    if(!policy||!priority)return EFAULT;
    thread_t thread=klj_thread_acquire(id);if(!thread)return errno;
    int host_policy=0,host_priority=0,error=scheduler_state(thread,&host_policy,&host_priority,quantum);
    if(!error){
        *policy=host_policy;*priority=host_policy?linux_priority(host_policy,host_priority):0;
        /* Retain the exact guest priority when range scaling is lossy, and the
         * Linux batch/idle/reset policy labels, only while kernel state agrees. */
        pthread_mutex_lock(&lock);
        for(identity *p=identities;p;p=p->next)if(p->port==thread&&p->schedule_valid&&p->host_policy==host_policy&&p->host_priority==host_priority){*policy=p->guest_policy;*priority=p->guest_priority;break;}
        pthread_mutex_unlock(&lock);
    }
    mach_port_deallocate(mach_task_self(),thread);return error;
}
int klj_thread_scheduler_set(int id,int policy,int priority){
    int selected=policy&~0x40000000;
    if(policy<0||(selected!=0&&selected!=1&&selected!=2&&selected!=3&&selected!=5)||
       ((selected==1||selected==2)?priority<1||priority>99:priority!=0))return EINVAL;
    if(id==0){id=klj_thread_id();if(id<0)return errno;}
    thread_t thread=klj_thread_acquire(id);if(!thread)return errno;
    int previous_nice=0;if(selected==0)klj_thread_priority_get(id,&previous_nice);
    integer_t host=selected==1?POLICY_FIFO:selected==2?POLICY_RR:POLICY_TIMESHARE;
    union {policy_timeshare_base_data_t normal;policy_fifo_base_data_t fifo;policy_rr_base_data_t rr;} request={0};
    request.normal.base_priority=selected==1||selected==2?native_priority(selected,priority):31;
    mach_msg_type_number_t size=selected==1?POLICY_FIFO_BASE_COUNT:selected==2?POLICY_RR_BASE_COUNT:POLICY_TIMESHARE_BASE_COUNT;
    int error=scheduling_error(thread_policy(thread,host,(policy_base_t)&request,size,TRUE));
    if(!error&&selected==0)error=klj_thread_priority_set(id,previous_nice);
    if(!error&&(selected==3||selected==5))error=klj_thread_priority_set(id,selected==5?19:5);
    int actual_policy=0,actual_priority=0;
    if(!error)error=scheduler_state(thread,&actual_policy,&actual_priority,NULL);
    if(!error){pthread_mutex_lock(&lock);for(identity *p=identities;p;p=p->next)if(p->port==thread){p->guest_policy=policy;p->guest_priority=priority;p->host_policy=actual_policy;p->host_priority=actual_priority;p->schedule_valid=1;break;}pthread_mutex_unlock(&lock);}
    mach_port_deallocate(mach_task_self(),thread);return error;
}
