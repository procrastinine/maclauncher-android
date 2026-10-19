#include "../thread-identity.h"
// One registry retains the actual Darwin thread and its guest attributes until
// join/detach. Mutex diagnostics and XR scheduling borrow the same owner.
typedef struct klj_thread {
    struct klj_thread *next;
    pthread_t thread;
    uint64_t tid;
    bionic_attr attributes;
    void *(*fn)(void *);
    void *arg;
    int running;
    char name[64];
    pthread_override_t boost;
} klj_thread;
static pthread_mutex_t klj_threads_lock=PTHREAD_MUTEX_INITIALIZER;
static klj_thread *klj_threads;
// ABI views borrow the canonical thread identity and retire with this owner.
// This notification carries no scheduling, joining or thread state of its own.
static void (*klj_thread_retired)(void *,void *);
static void *klj_thread_retired_context;
void klj_pthread_observe_retirement(void (*callback)(void *,void *),void *context){
    pthread_mutex_lock(&klj_threads_lock);klj_thread_retired=callback;klj_thread_retired_context=context;pthread_mutex_unlock(&klj_threads_lock);
}
static klj_thread *klj_thread_find(pthread_t thread){for(klj_thread *p=klj_threads;p;p=p->next)if(p->thread==thread)return p;return NULL;}
static void klj_thread_remove(klj_thread *entry){
    for(klj_thread **p=&klj_threads;*p;p=&(*p)->next)if(*p==entry){*p=entry->next;if(entry->boost)pthread_override_qos_class_end_np(entry->boost);if(klj_thread_retired&&entry->thread)klj_thread_retired(klj_thread_retired_context,entry->thread);free(entry);return;}
}
static void thread_register(void *self){
    pthread_mutex_lock(&klj_threads_lock);klj_thread *p=klj_thread_find(self);
    if(p){pthread_getname_np(pthread_self(),p->name,sizeof(p->name));p->tid=klj_thread_id();}
    pthread_mutex_unlock(&klj_threads_lock);
}
static void thread_unregister(void *self){
    pthread_mutex_lock(&klj_threads_lock);klj_thread *p=klj_thread_find(self);
    if(p){p->running=0;if(p->attributes.flags&BIONIC_ATTR_DETACHED)klj_thread_remove(p);}
    pthread_mutex_unlock(&klj_threads_lock);
}
static const char *thread_name_of(void *self){
    static _Thread_local char name[64];pthread_mutex_lock(&klj_threads_lock);klj_thread *p=klj_thread_find(self);
    if(p)memcpy(name,p->name,sizeof(name));pthread_mutex_unlock(&klj_threads_lock);return p?name:NULL;
}
static int thread_is_alive(void *self){pthread_mutex_lock(&klj_threads_lock);klj_thread *p=klj_thread_find(self);int live=p&&p->running;pthread_mutex_unlock(&klj_threads_lock);return live;}
void kl_pthread_boost_qos(uint64_t tid){
    if(!kl_env_on("KL_XR_THREAD_QOS",1))return;
    uint64_t self=klj_thread_id();
    if(tid==self){pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);return;}
    pthread_mutex_lock(&klj_threads_lock);
    for(klj_thread *p=klj_threads;p;p=p->next)if(p->running&&p->tid==tid){if(!p->boost)p->boost=pthread_override_qos_class_start_np(p->thread,QOS_CLASS_USER_INTERACTIVE,0);break;}
    pthread_mutex_unlock(&klj_threads_lock);
}
static int klj_policy_to_host(int policy){switch(policy){case 0:return SCHED_OTHER;case 1:return SCHED_FIFO;case 2:return SCHED_RR;default:return -1;}}
static int klj_policy_to_guest(int policy){switch(policy){case SCHED_OTHER:return 0;case SCHED_FIFO:return 1;case SCHED_RR:return 2;default:return -1;}}
static int klj_priority_to_host(int guestPolicy,int priority,int *native){
    int policy=klj_policy_to_host(guestPolicy);if(policy<0)return EINVAL;
    if(!guestPolicy){if(priority)return EINVAL;pthread_attr_t a;struct sched_param p;pthread_attr_init(&a);int status=pthread_attr_getschedparam(&a,&p);pthread_attr_destroy(&a);if(status)return status;*native=p.sched_priority;return 0;}
    if(priority<1||priority>99)return EINVAL;
    int min=sched_get_priority_min(policy),max=sched_get_priority_max(policy);if(min<0||max<0)return errno;
    *native=min+(priority-1)*(max-min)/98;return 0;
}
static int klj_priority_to_guest(int policy,int priority){
    if(policy==SCHED_OTHER)return 0;int min=sched_get_priority_min(policy),max=sched_get_priority_max(policy);
    return min>=0&&max>min?1+(priority-min)*98/(max-min):1;
}
int klb_pthread_getschedparam(void *thread,int *policy,void *value){
    if(!policy||!value)return 22;int nativePolicy;struct sched_param nativeParam;
    int status=pthread_getschedparam(thread,&nativePolicy,&nativeParam);if(status)return px(status);
    int guest=klj_policy_to_guest(nativePolicy);if(guest<0)return 95;
    *policy=guest;((klj_linux_sched_param*)value)->sched_priority=klj_priority_to_guest(nativePolicy,nativeParam.sched_priority);return 0;
}
int klb_pthread_setschedparam(void *thread,int policy,const void *value){
    if(!value)return 22;struct sched_param parameter={0};int nativePolicy=klj_policy_to_host(policy);
    int error=klj_priority_to_host(policy,((const klj_linux_sched_param*)value)->sched_priority,&parameter.sched_priority);
    return error?px(error):px(pthread_setschedparam(thread,nativePolicy,&parameter));
}
int klj_pthread_getattr_np(pthread_t thread,bionic_attr *attributes){
    pthread_mutex_lock(&klj_threads_lock);klj_thread *owner=klj_thread_find(thread);
    if(owner)*attributes=owner->attributes;else{klb_pthread_attr_init(attributes);attributes->guard_size=0;}
    size_t size=pthread_get_stacksize_np(thread);void *high=pthread_get_stackaddr_np(thread);
    if(!size||!high){pthread_mutex_unlock(&klj_threads_lock);return 3;}
    attributes->stack_size=size;attributes->stack_base=(char*)high-size;
    if(!owner){int policy;klj_linux_sched_param param;if(!klb_pthread_getschedparam(thread,&policy,&param)){attributes->sched_policy=policy;attributes->sched_priority=param.sched_priority;}}
    pthread_mutex_unlock(&klj_threads_lock);return 0;
}
static int klj_thread_attributes(pthread_attr_t *native,const bionic_attr *guest,bionic_attr *retained){
    if(guest)*retained=*guest;else klb_pthread_attr_init(retained);
    int error=0;
    if(guest){
        if(guest->stack_base)error=pthread_attr_setstack(native,guest->stack_base,guest->stack_size);
        else if(guest->stack_size)error=klj_pthread_set_guest_stacksize(native,guest->stack_size);
        if(!error)error=pthread_attr_setguardsize(native,guest->stack_base?0:guest->guard_size);
        if(!error)error=pthread_attr_setdetachstate(native,(guest->flags&BIONIC_ATTR_DETACHED)?PTHREAD_CREATE_DETACHED:PTHREAD_CREATE_JOINABLE);
        int inheritance=1;klb_pthread_attr_getinheritsched(guest,&inheritance);
        if(!error)error=pthread_attr_setinheritsched(native,inheritance?PTHREAD_INHERIT_SCHED:PTHREAD_EXPLICIT_SCHED);
        if(!error&&!inheritance){
            struct sched_param parameter={0};int policy=klj_policy_to_host(guest->sched_policy);
            error=klj_priority_to_host(guest->sched_policy,guest->sched_priority,&parameter.sched_priority);
            if(!error)error=pthread_attr_setschedpolicy(native,policy);
            if(!error)error=pthread_attr_setschedparam(native,&parameter);
        }
    }
    if(!error){size_t guard;error=pthread_attr_getguardsize(native,&guard);if(!error)retained->guard_size=guard;}
    return error;
}
static int klj_thread_join(pthread_t thread,void **result){
    int status=pthread_join(thread,result);if(!status){pthread_mutex_lock(&klj_threads_lock);klj_thread *p=klj_thread_find(thread);if(p)klj_thread_remove(p);pthread_mutex_unlock(&klj_threads_lock);}return px(status);
}
static int klj_thread_detach(pthread_t thread){
    pthread_mutex_lock(&klj_threads_lock);int status=pthread_detach(thread);klj_thread *p=klj_thread_find(thread);
    if(!status&&p){p->attributes.flags|=BIONIC_ATTR_DETACHED;if(!p->running)klj_thread_remove(p);}
    pthread_mutex_unlock(&klj_threads_lock);return px(status);
}
