#include "looper-host.h"
#include "jni-invocation.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int kl_errno_to_linux(int),kl_errno_from_linux(int);
static KljBionicLooperOwner owner;static JavaVM *vm;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic int ready;
static ALooper*(*p_forThread)(void);static ALooper*(*p_prepare)(int);
static void(*p_acquire)(ALooper*),(*p_release)(ALooper*),(*p_wake)(ALooper*);
static int(*p_add)(ALooper*,int,int,int,ALooper_callbackFunc,void*),(*p_remove)(ALooper*,int);
static int(*p_pollOnce)(int,int*,int*,void**),(*p_pollAll)(int,int*,int*,void**);
typedef struct Thread Thread;typedef struct Registration Registration;
struct Registration{Registration*next;Thread*thread;int guest,native,ident,active,callbacks;void*lease;uintptr_t caller;ALooper_callbackFunc callback;void*data;};
struct Thread{Thread*next;pthread_t physical;ALooper*looper;Registration*registrations;unsigned references;int retired,physicalEnd,dispatching;};
/* This list contains only existing host Looper ownership; no guest image or FD
 * authority is inferred here. Stable nodes protect original poll snapshots
 * across concurrent remove. Original owner still owns polling/refcounts. */
static Thread *threads;static _Thread_local Thread *current;
/* Native Bionic pthreads have no JVMTI ThreadEnd. This key retires only their
 * existing Looper registrations on physical exit; it never attaches to Java. */
static pthread_key_t physical_key;static int physical_key_ready;
static void physical_end(void*unused);
static int unavailable(const char*name,int status){fprintf(stderr,"Android NDK operation unavailable: %s (Linux status %d)\n",name,status);return status;}
static void release_pin(Registration*r){void*lease=r->lease;r->lease=NULL;if(lease){int64_t status=owner.poll->release(lease);if(status)fprintf(stderr,"Android Looper descriptor retirement failed: %lld\n",(long long)status);}}
static Thread*find(ALooper*p){for(Thread*t=threads;t;t=t->next)if(t->looper==p)return t;return NULL;}
static Thread*remember(ALooper*p){
 if(!p||!physical_key_ready)return NULL;
 pthread_mutex_lock(&lock);Thread*t=find(p);int created=0;
 if(!t){t=calloc(1,sizeof*t);if(t){t->looper=p;t->physical=pthread_self();p_acquire(p);t->next=threads;threads=t;created=1;}}
 if(t&&pthread_equal(t->physical,pthread_self())&&!t->retired&&!pthread_setspecific(physical_key,t))current=t;
 else{if(created){threads=t->next;p_release(p);free(t);}t=NULL;errno=ENOMEM;}
 pthread_mutex_unlock(&lock);return t;
}
static void discard_inactive(Thread*t){/* A live physical owner can still have original poll snapshots. */
 if(!t||t->dispatching||(!t->physicalEnd&&!pthread_equal(t->physical,pthread_self())))return;Registration**next=&t->registrations;
 while(*next){Registration*r=*next;if(!r->active&&!r->callbacks){*next=r->next;release_pin(r);free(r);}else next=&r->next;}
}
static int callback(int fd,int events,void*raw){
 Registration*r=raw;pthread_mutex_lock(&lock);Thread*t=r->thread;
 if(!r->active||t->retired||r->native!=fd||!pthread_equal(t->physical,pthread_self())){r->active=0;if(!r->callbacks)release_pin(r);pthread_mutex_unlock(&lock);return 0;}
 r->callbacks++;t->dispatching++;ALooper_callbackFunc function=r->callback;void*data=r->data;int guest=r->guest;uintptr_t caller=r->caller;pthread_mutex_unlock(&lock);
 JNIEnv*env=NULL;int keep=0;int64_t status=-1;
 jint attached=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
 if((attached==JNI_OK&&env&&!(*env)->ExceptionCheck(env))||(attached==JNI_EDETACHED&&!env)){
  KljCallPacket packet={.size=sizeof packet,.version=KLJ_CALL_PACKET_VERSION,.target=(uintptr_t)function,.x={(uint64_t)(int64_t)guest,(uint64_t)(int64_t)events,(uintptr_t)data}};
  int saved=errno;status=owner.invoke(owner.context,env,caller,&packet);errno=saved;
  if(!status&&!packet.status&&(!env||!(*env)->ExceptionCheck(env)))keep=(int)packet.result_x[0];
 }
 if(status)unavailable("ALooper_addFd callback",(int)status);
 pthread_mutex_lock(&lock);r->callbacks--;t->dispatching--;if(!keep||t->retired)r->active=0;if(!r->active)release_pin(r);keep=r->active?keep:0;pthread_mutex_unlock(&lock);return keep;
}
static ALooper*for_thread(void){ALooper*p=p_forThread?p_forThread():NULL;return remember(p)?p:NULL;}
static ALooper*prepare(int opts){if(!p_prepare||klj_jni_thread_retiring()){errno=ENOSYS;return NULL;}ALooper*p=p_prepare(opts);return remember(p)?p:NULL;}
static int known(ALooper*p,int allow_retired){Thread*t=find(p);return t&&(!t->physicalEnd||t->references)&&(!t->retired||allow_retired);}
static void reap_thread(Thread*t){if(!t||!t->physicalEnd||t->references||t->registrations||t->dispatching)return;Thread**next=&threads;while(*next&&*next!=t)next=&(*next)->next;if(*next)*next=t->next;free(t);}
static void retire_registrations(Thread*t){for(Registration*r=t->registrations;r;r=r->next){if(r->active&&p_remove)p_remove(t->looper,r->native);r->active=0;if(!r->callbacks)release_pin(r);}}
static void acquire(ALooper*p){if(!p_acquire){errno=ENOSYS;unavailable("ALooper_acquire",-38);return;}pthread_mutex_lock(&lock);Thread*t=find(p);if(t&&(!t->physicalEnd||t->references)){t->references++;p_acquire(p);}else unavailable("ALooper_acquire",-22);pthread_mutex_unlock(&lock);}
static void release(ALooper*p){if(!p_release){errno=ENOSYS;unavailable("ALooper_release",-38);return;}pthread_mutex_lock(&lock);Thread*t=find(p);if(t&&t->references){t->references--;if(t->retired&&!t->references){retire_registrations(t);discard_inactive(t);}p_release(p);reap_thread(t);}else unavailable("ALooper_release",-22);pthread_mutex_unlock(&lock);}
static void wake(ALooper*p){if(!p_wake){errno=ENOSYS;unavailable("ALooper_wake",-38);return;}pthread_mutex_lock(&lock);if(known(p,1))p_wake(p);else unavailable("ALooper_wake",-22);pthread_mutex_unlock(&lock);}
static int add(JNIEnv*env,uintptr_t caller,ALooper*p,int guest,int ident,int events,ALooper_callbackFunc function,void*data){
 (void)env;if(!p_add||!p_remove||guest<0||(events&~31)||(!function&&ident<0)||klj_jni_thread_retiring()){errno=EINVAL;return -1;}
 Registration*r=calloc(1,sizeof*r);if(!r){errno=ENOMEM;return -1;}
 int64_t status=owner.poll->borrow(guest,&r->native,&r->lease);if(status){free(r);errno=kl_errno_from_linux((int)-status);unavailable("ALooper_addFd descriptor",(int)status);return -1;}
 r->guest=guest;r->ident=ident;r->callback=function;r->caller=caller;r->data=data;
 pthread_mutex_lock(&lock);Thread*t=find(p);if(!t||t->retired){pthread_mutex_unlock(&lock);release_pin(r);free(r);errno=EINVAL;return -1;}
 r->thread=t;r->active=1;
 int result=p_add(p,r->native,ident,events,function?callback:NULL,r);
 if(result==1){for(Registration*old=t->registrations;old;old=old->next)if(old->active&&old->guest==guest){p_remove(p,old->native);old->active=0;if(!old->callbacks)release_pin(old);}r->next=t->registrations;t->registrations=r;}
 pthread_mutex_unlock(&lock);if(result!=1){release_pin(r);free(r);}return result;
}
static int remove_fd(ALooper*p,int guest){if(!p_remove){errno=ENOSYS;return -1;}pthread_mutex_lock(&lock);Thread*t=find(p);int result=0;if(!t){pthread_mutex_unlock(&lock);return -1;}
 for(Registration*r=t->registrations;r;r=r->next)if(r->active&&r->guest==guest){int removed=p_remove(p,r->native);if(removed<0){result=removed;break;}r->active=0;if(!r->callbacks)release_pin(r);if(removed)result=removed;}
 pthread_mutex_unlock(&lock);return result;
}
static int poll_loop(int all,int timeout,int*outFd,int*outEvents,void**outData){
 int(*poller)(int,int*,int*,void**)=all?p_pollAll:p_pollOnce;if(!poller)return ALOOPER_POLL_ERROR;
 Thread*t=current;if(!t||t->retired||!pthread_equal(t->physical,pthread_self()))return ALOOPER_POLL_ERROR;
 pthread_mutex_lock(&lock);discard_inactive(t);t->dispatching++;pthread_mutex_unlock(&lock);
 int fd=-1,events=0;void*raw=NULL;int result=poller(timeout,&fd,&events,&raw);
 pthread_mutex_lock(&lock);t->dispatching--;if(result>=0){Registration*r=t->registrations;while(r&&r!=raw)r=r->next;if(!r){result=ALOOPER_POLL_ERROR;}else{if(outFd)*outFd=r->guest;if(outEvents)*outEvents=events;if(outData)*outData=r->data;}}
 discard_inactive(t);pthread_mutex_unlock(&lock);return result;
}
static void retire_owner(Thread*t){
 /* Retirement ends guest dispatch authority, not an externally acquired
  * Looper's descriptor lifetime. Its holder can still removeFd from another
  * thread; the last release retires registrations it did not remove. */
 t->retired=1;if(!t->references)retire_registrations(t);discard_inactive(t);
}
void klj_bionic_looper_retire_current(void){Thread*t=current;if(!t)return;pthread_mutex_lock(&lock);retire_owner(t);pthread_mutex_unlock(&lock);}
static void java_end(void*unused){(void)unused;klj_bionic_looper_retire_current();}
/* The key retains the actual owner: Darwin may release this image's TLV
 * storage before running an unrelated pthread-key destructor. Its independent
 * Looper reference also survives the original Looper TLS destructor order. */
static void physical_end(void*value){
 Thread*t=value;if(!t&&physical_key_ready)t=pthread_getspecific(physical_key);if(!t)return;
 pthread_setspecific(physical_key,NULL);
 pthread_mutex_lock(&lock);retire_owner(t);t->physicalEnd=1;current=NULL;p_release(t->looper);reap_thread(t);pthread_mutex_unlock(&lock);
}
static KljJniThreadObserver observer={.javaEnd=java_end,.physicalEnd=physical_end};
