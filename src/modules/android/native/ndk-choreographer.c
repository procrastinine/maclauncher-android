/* Native display callbacks share the window's CoreVideo clock and Klepton's
 * existing ALooper. No independent timer or Java framework client is created. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include "native-clock/clock-domain.h"

typedef struct AChoreographer AChoreographer;
typedef struct AChoreographerFrameCallbackData {int64_t frame,sequence,output,deadline;} AChoreographerFrameCallbackData;
typedef void (*FrameCallback)(int64_t,void*);
typedef void (*VsyncCallback)(const AChoreographerFrameCallbackData*,void*);
typedef void (*RefreshCallback)(int64_t,void*);
typedef struct Frame {struct Frame *next;int64_t due;FrameCallback frame;VsyncCallback vsync;void *data;int ready;AChoreographerFrameCallbackData timing;} Frame;
typedef struct Refresh {struct Refresh *next;RefreshCallback call;void *data;int pending,active,removed,references;} Refresh;
struct AChoreographer {AChoreographer *next;void *looper;pthread_t thread;int pipe[2],closed;Frame *frames,**tail;Refresh *refresh;};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static pthread_key_t key;
static AChoreographer *owners;
static int stopped,active,keyReady;
static int64_t interval;
static void (*wakeup)(void*);static void *wakeContext;
/* The existing display trace also describes native callback progress. Counters
 * share the callback owner's lock; formatting and I/O happen after releasing it. */
static pthread_once_t trace_once=PTHREAD_ONCE_INIT;
static int trace_enabled;
static uint64_t trace_posts,trace_dispatches,trace_ticks,trace_ready;
static int64_t trace_last_emit,trace_last_due;
typedef struct {int valid,owners,queued,ready,executing,source;uint64_t posts,dispatches,ticks,marked;int64_t elapsed,callback,lastDue,oldestDue;} Trace;
static void initialize_trace(void){const char *value=getenv("KL_DISPLAY_TRACE");trace_enabled=value&&*value&&strcmp(value,"0");}
static Trace trace_locked(int64_t callback){
 Trace result={0};if(!trace_enabled)return result;
 trace_ticks++;if(!trace_last_emit){trace_last_emit=callback;return result;}
 if(callback<trace_last_emit||callback-trace_last_emit<2000000000LL)return result;
 result=(Trace){.valid=1,.executing=active,.source=wakeup!=NULL,.posts=trace_posts,.dispatches=trace_dispatches,.ticks=trace_ticks,.marked=trace_ready,.elapsed=callback-trace_last_emit,.callback=callback,.lastDue=trace_last_due};trace_last_emit=callback;
 for(AChoreographer *owner=owners;owner;owner=owner->next){result.owners++;for(Frame *frame=owner->frames;frame;frame=frame->next){result.queued++;result.ready+=frame->ready;if(!result.oldestDue||frame->due<result.oldestDue)result.oldestDue=frame->due;}}
 return result;
}
static void report_trace(Trace value){if(value.valid)fprintf(stderr,"[android-ndk-display] seconds=%.3f owners=%d queued=%d ready=%d active=%d source=%d ticks=%llu posted=%llu marked=%llu dispatched=%llu callback-ns=%lld last-due-ns=%lld oldest-due-ns=%lld\n",value.elapsed/1e9,value.owners,value.queued,value.ready,value.executing,value.source,(unsigned long long)value.ticks,(unsigned long long)value.posts,(unsigned long long)value.marked,(unsigned long long)value.dispatches,(long long)value.callback,(long long)value.lastDue,(long long)value.oldestDue);}
static void *(*forThread)(void);
static void (*acquire)(void*),(*release)(void*);
static int (*addFd)(void*,int,int,int,int(*)(int,int,void*),void*),(*removeFd)(void*,int);
extern void *kl_upstream_ndk_lookup(const char*);
/* Android frame deadlines and CVDisplayLink timestamps share Mach uptime.
 * Darwin CLOCK_MONOTONIC includes suspended time and cannot be compared with
 * the display mailbox after the Mac has slept. */
static int64_t now(void){struct timespec value;klj_clock_read(1,&value);return (int64_t)value.tv_sec*1000000000+value.tv_nsec;}
static void signal_owner(AChoreographer *owner){char byte=1;ssize_t result;do{result=write(owner->pipe[1],&byte,1);}while(result<0&&errno==EINTR);}
static void signal_display(void){if(wakeup&&!stopped)wakeup(wakeContext);}
static void clear_frames(AChoreographer *owner){while(owner->frames){Frame *frame=owner->frames;owner->frames=frame->next;free(frame);}owner->tail=&owner->frames;}
static void retire(void *value){
 AChoreographer *owner=value;pthread_mutex_lock(&lock);
 AChoreographer **link=&owners;while(*link&&*link!=owner)link=&(*link)->next;if(*link)*link=owner->next;
 owner->closed=1;clear_frames(owner);while(owner->refresh){Refresh *entry=owner->refresh;owner->refresh=entry->next;free(entry);}signal_display();pthread_mutex_unlock(&lock);
 removeFd(owner->looper,owner->pipe[0]);close(owner->pipe[0]);close(owner->pipe[1]);release(owner->looper);free(owner);
}
static void initialize(void){
 pthread_once(&trace_once,initialize_trace);
 forThread=kl_upstream_ndk_lookup("ALooper_forThread");acquire=kl_upstream_ndk_lookup("ALooper_acquire");release=kl_upstream_ndk_lookup("ALooper_release");addFd=kl_upstream_ndk_lookup("ALooper_addFd");removeFd=kl_upstream_ndk_lookup("ALooper_removeFd");
 keyReady=forThread&&acquire&&release&&addFd&&removeFd&&pthread_key_create(&key,retire)==0;
}
typedef struct {Refresh *refresh;Frame *frame;} Dispatch;
static void complete_dispatch(void *value){Dispatch *work=value;pthread_mutex_lock(&lock);active--;if(work->refresh){work->refresh->active=0;pthread_cond_broadcast(&changed);if(!--work->refresh->references)free(work->refresh);}pthread_mutex_unlock(&lock);free(work->frame);}
static int dispatch(int fd,int events,void *value){
 (void)events;AChoreographer *owner=value;char bytes[64];while(read(fd,bytes,sizeof bytes)>0){}
 for(;;){
  pthread_mutex_lock(&lock);if(stopped||owner->closed||!wakeup){pthread_mutex_unlock(&lock);break;}
  Refresh *refresh=NULL;for(Refresh *entry=owner->refresh;entry;entry=entry->next)if(entry->pending&&!entry->removed){refresh=entry;entry->pending=0;entry->active=1;entry->references++;break;}
  Frame *frame=NULL;
  if(!refresh){Frame **link=&owner->frames;while(*link&&!(*link)->ready)link=&(*link)->next;if(*link){frame=*link;*link=frame->next;if(owner->tail==&frame->next)owner->tail=link;}}
  if(!refresh&&!frame){pthread_mutex_unlock(&lock);break;}
  active++;if(trace_enabled&&frame)trace_dispatches++;int64_t period=interval;pthread_mutex_unlock(&lock);
  Dispatch work={refresh,frame};pthread_cleanup_push(complete_dispatch,&work);
  if(refresh)refresh->call(period,refresh->data);else if(frame->vsync)frame->vsync(&frame->timing,frame->data);else frame->frame(frame->timing.frame,frame->data);
  pthread_cleanup_pop(1);
 }
 return 1;
}
AChoreographer *AChoreographer_getInstance(void){
 pthread_once(&once,initialize);if(!keyReady)return NULL;AChoreographer *owner=pthread_getspecific(key);if(owner)return owner;
 void *looper=forThread();if(!looper)return NULL;
 owner=calloc(1,sizeof *owner);if(!owner)return NULL;
 if(pipe(owner->pipe)){free(owner);return NULL;}
 for(int i=0;i<2;i++){fcntl(owner->pipe[i],F_SETFL,fcntl(owner->pipe[i],F_GETFL,0)|O_NONBLOCK);fcntl(owner->pipe[i],F_SETFD,FD_CLOEXEC);}
 owner->looper=looper;owner->thread=pthread_self();owner->tail=&owner->frames;acquire(looper);
 if(addFd(looper,owner->pipe[0],-1,1,dispatch,owner)<0||pthread_setspecific(key,owner)){removeFd(looper,owner->pipe[0]);close(owner->pipe[0]);close(owner->pipe[1]);release(looper);free(owner);return NULL;}
 pthread_mutex_lock(&lock);owner->closed=stopped;owner->next=owners;owners=owner;pthread_mutex_unlock(&lock);return owner;
}
static void post(AChoreographer *owner,FrameCallback frame,VsyncCallback vsync,void *data,int64_t delay){
 if(!owner||(!frame&&!vsync))return;Frame *entry=calloc(1,sizeof *entry);if(!entry){fputs("Android native display callback allocation failed\n",stderr);return;}
 int64_t current=now();entry->due=delay>0&&delay>(INT64_MAX-current)/1000000?INT64_MAX:current+(delay>0?delay:0)*1000000;entry->frame=frame;entry->vsync=vsync;entry->data=data;
 pthread_mutex_lock(&lock);if(stopped||owner->closed)free(entry);else{*owner->tail=entry;owner->tail=&entry->next;if(trace_enabled){trace_posts++;trace_last_due=entry->due;}signal_display();}pthread_mutex_unlock(&lock);
}
void AChoreographer_postFrameCallback(AChoreographer *owner,void(*callback)(long,void*),void *data){post(owner,(FrameCallback)callback,NULL,data,0);}
void AChoreographer_postFrameCallbackDelayed(AChoreographer *owner,void(*callback)(long,void*),void *data,long delay){post(owner,(FrameCallback)callback,NULL,data,delay);}
void AChoreographer_postFrameCallback64(AChoreographer *owner,FrameCallback callback,void *data){post(owner,callback,NULL,data,0);}
void AChoreographer_postFrameCallbackDelayed64(AChoreographer *owner,FrameCallback callback,void *data,uint32_t delay){post(owner,callback,NULL,data,delay);}
void AChoreographer_postVsyncCallback(AChoreographer *owner,VsyncCallback callback,void *data){post(owner,NULL,callback,data,0);}
void AChoreographer_registerRefreshRateCallback(AChoreographer *owner,RefreshCallback callback,void *data){
 if(!owner||!callback)return;pthread_mutex_lock(&lock);if(stopped||owner->closed){pthread_mutex_unlock(&lock);return;}
 Refresh *entry;for(entry=owner->refresh;entry;entry=entry->next)if(entry->call==callback&&entry->data==data)break;
 if(!entry){entry=calloc(1,sizeof *entry);if(entry){entry->references=1;entry->call=callback;entry->data=data;entry->next=owner->refresh;owner->refresh=entry;}}
 if(entry){entry->removed=0;entry->pending=interval>0;if(entry->pending)signal_owner(owner);signal_display();}pthread_mutex_unlock(&lock);
}
void AChoreographer_unregisterRefreshRateCallback(AChoreographer *owner,RefreshCallback callback,void *data){
 if(!owner||!callback)return;pthread_mutex_lock(&lock);Refresh **link=&owner->refresh;while(*link&&((*link)->call!=callback||(*link)->data!=data))link=&(*link)->next;
 Refresh *entry=*link;if(entry){*link=entry->next;entry->removed=1;entry->pending=0;while(entry->active&&!pthread_equal(owner->thread,pthread_self()))pthread_cond_wait(&changed,&lock);
  /* The removed registry reference keeps concurrent dispatch alive. */
  if(!--entry->references)free(entry);}
 pthread_mutex_unlock(&lock);
}
int64_t AChoreographerFrameCallbackData_getFrameTimeNanos(const AChoreographerFrameCallbackData *data){return data->frame;}
size_t AChoreographerFrameCallbackData_getFrameTimelinesLength(const AChoreographerFrameCallbackData *data){(void)data;return 1;}
size_t AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(const AChoreographerFrameCallbackData *data){(void)data;return 0;}
int64_t AChoreographerFrameCallbackData_getFrameTimelineVsyncId(const AChoreographerFrameCallbackData *data,size_t index){return index==0?data->sequence:-1;}
int64_t AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(const AChoreographerFrameCallbackData *data,size_t index){return index==0?data->output:-1;}
int64_t AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(const AChoreographerFrameCallbackData *data,size_t index){return index==0?data->deadline:-1;}
void klj_ndk_choreographer_set_wakeup(void(*callback)(void*),void *context){pthread_once(&trace_once,initialize_trace);pthread_mutex_lock(&lock);wakeup=callback;wakeContext=context;if(callback){for(AChoreographer *owner=owners;owner;owner=owner->next)for(Refresh *entry=owner->refresh;entry;entry=entry->next)if(entry->pending){signal_owner(owner);break;}signal_display();}pthread_mutex_unlock(&lock);}
int klj_ndk_choreographer_needs_frames(void){
 /* Delivery owns demand through the queued, executing and reposted states.
  * Dropping ready callbacks can stop the hardware clock between publishing a
  * tick and ALooper dispatch, repeatedly restarting the source each frame.
  * Read active and queued work together: separate snapshots can miss the
  * transition from an executing callback to its next posted callback. */
 int result=0;int64_t current=now();pthread_mutex_lock(&lock);if(!stopped){result=active>0;for(AChoreographer *owner=owners;owner&&!result;owner=owner->next){for(Frame *frame=owner->frames;frame;frame=frame->next)if(frame->ready||frame->due<=current){result=1;break;}if(!interval)for(Refresh *entry=owner->refresh;entry;entry=entry->next)if(!entry->removed){result=1;break;}}}pthread_mutex_unlock(&lock);return result;
}
int klj_ndk_choreographer_wait_millis(int maximum){
 int64_t current=now();pthread_mutex_lock(&lock);if(!stopped)for(AChoreographer *owner=owners;owner;owner=owner->next)for(Frame *frame=owner->frames;frame;frame=frame->next)if(!frame->ready&&frame->due>current){int64_t delay=(frame->due-current+999999)/1000000;if(delay<maximum||maximum<0)maximum=delay>INT_MAX?INT_MAX:(int)delay;}pthread_mutex_unlock(&lock);return maximum;
}
static void refresh_period_locked(int64_t period){if(period<=0||interval==period)return;interval=period;for(AChoreographer *owner=owners;owner;owner=owner->next){int ready=0;if(owner->closed)continue;for(Refresh *entry=owner->refresh;entry;entry=entry->next)if(!entry->removed){entry->pending=1;ready=1;}if(ready)signal_owner(owner);}}
void klj_ndk_choreographer_refresh_period(int64_t period){pthread_mutex_lock(&lock);if(!stopped)refresh_period_locked(period);pthread_mutex_unlock(&lock);}
void klj_ndk_choreographer_tick(int64_t sequence,int64_t callback,int64_t output,int64_t period){
 pthread_mutex_lock(&lock);if(!stopped&&wakeup){if(!interval)refresh_period_locked(period);for(AChoreographer *owner=owners;owner;owner=owner->next){if(owner->closed)continue;int ready=0;
  for(Frame *frame=owner->frames;frame;frame=frame->next)if(!frame->ready&&frame->due<=callback){frame->ready=1;frame->timing=(AChoreographerFrameCallbackData){callback,sequence,output,output};ready=1;if(trace_enabled)trace_ready++;}
  if(ready)signal_owner(owner);
 }}Trace trace=trace_locked(callback);pthread_mutex_unlock(&lock);report_trace(trace);
}
int klj_ndk_choreographer_owns_fd(int fd){pthread_mutex_lock(&lock);int result=0;for(AChoreographer *owner=owners;owner;owner=owner->next)if(owner->pipe[0]==fd){result=1;break;}pthread_mutex_unlock(&lock);return result;}
int klj_ndk_choreographer_inflight(void){pthread_mutex_lock(&lock);int result=active;pthread_mutex_unlock(&lock);return result;}
void klj_ndk_choreographer_detach(void){pthread_mutex_lock(&lock);wakeup=NULL;wakeContext=NULL;interval=0;for(AChoreographer *owner=owners;owner;owner=owner->next){for(Frame *entry=owner->frames;entry;entry=entry->next)entry->ready=0;for(Refresh *entry=owner->refresh;entry;entry=entry->next)entry->pending=0;}pthread_mutex_unlock(&lock);}
void klj_ndk_choreographer_shutdown(void){pthread_mutex_lock(&lock);stopped=1;wakeup=NULL;wakeContext=NULL;for(AChoreographer *owner=owners;owner;owner=owner->next){owner->closed=1;clear_frames(owner);for(Refresh *entry=owner->refresh;entry;entry=entry->next)entry->pending=0;signal_owner(owner);}pthread_mutex_unlock(&lock);}
void *klj_ndk_choreographer_lookup(const char *name){
#define ENTRY(function) if(!strcmp(name,#function))return (void*)function
 ENTRY(AChoreographer_getInstance);ENTRY(AChoreographer_postFrameCallback);ENTRY(AChoreographer_postFrameCallbackDelayed);ENTRY(AChoreographer_postFrameCallback64);ENTRY(AChoreographer_postFrameCallbackDelayed64);ENTRY(AChoreographer_postVsyncCallback);ENTRY(AChoreographer_registerRefreshRateCallback);ENTRY(AChoreographer_unregisterRefreshRateCallback);ENTRY(AChoreographerFrameCallbackData_getFrameTimeNanos);ENTRY(AChoreographerFrameCallbackData_getFrameTimelinesLength);ENTRY(AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex);ENTRY(AChoreographerFrameCallbackData_getFrameTimelineVsyncId);ENTRY(AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos);ENTRY(AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos);
#undef ENTRY
 return NULL;
}
