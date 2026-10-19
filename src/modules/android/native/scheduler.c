#include "scheduler.h"
#include "host-callbacks.h"
#include "maclauncher_android_bridge_FrameWakeup.h"
#include "clock/display-clock.h"
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
extern void klj_ndk_choreographer_set_wakeup(void(*)(void*),void*);
extern int klj_ndk_choreographer_needs_frames(void);
extern int klj_ndk_choreographer_wait_millis(int);
extern void klj_ndk_choreographer_tick(int64_t,int64_t,int64_t,int64_t);
extern int klj_ndk_choreographer_inflight(void);
extern void klj_ndk_choreographer_detach(void);
struct KLHostScheduler {
 JNIEnv* owner;JavaVM* vm;KLHostCallbacks* callbacks;jthrowable dispatcherFailure;
 KLDisplayClock* clock;semaphore_t semaphore;pthread_t dispatcher;Uint32 eventType;
 pthread_mutex_t sourceLock;
 atomic_int stopping,pending,eventPending,error,exited,threadStarted,forceFrames;int stopped,retired;
};
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Realtime signal flags must be lock-free");
/* One event ID and one active native window in this process's SDL lifetime.
 * The host runs one game per JVM; SDL_Quit occurs only at final host teardown.
 * Failed window creation/rebinding never registers another permanent ID. */
static pthread_mutex_t eventLock=PTHREAD_MUTEX_INITIALIZER;
static Uint32 processEventType;
static KLHostScheduler* eventOwner;
static KLHostScheduler* activeScheduler;
static void fail(JNIEnv* env,const char* text){if((*env)->ExceptionCheck(env))return;jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");if(type){(*env)->ThrowNew(env,type,text);(*env)->DeleteLocalRef(env,type);}}
static int owns(JNIEnv* env,KLHostScheduler* s){if(s&&s->owner==env)return 1;fail(env,"Android display scheduler owner is unavailable");return 0;}
static void sleep_one_ms(void){struct timespec wait={0,1000000};nanosleep(&wait,NULL);}
static int acquire_event(JNIEnv* env,KLHostScheduler* s){
 pthread_mutex_lock(&eventLock);
 if(eventOwner){pthread_mutex_unlock(&eventLock);fail(env,"Another Android display scheduler owns the event source");return 0;}
 if(!processEventType)processEventType=SDL_RegisterEvents(1);
 if(!processEventType){pthread_mutex_unlock(&eventLock);fail(env,"Android display wake event could not be registered");return 0;}
 eventOwner=s;s->eventType=processEventType;pthread_mutex_unlock(&eventLock);return 1;
}
static void release_event(KLHostScheduler* s){
 pthread_mutex_lock(&eventLock);
 if(eventOwner==s){SDL_FlushEvent(processEventType);eventOwner=NULL;s->eventType=0;}
 pthread_mutex_unlock(&eventLock);
}
static void release_scheduler(KLHostScheduler* s){pthread_mutex_lock(&eventLock);if(activeScheduler==s)activeScheduler=NULL;pthread_mutex_unlock(&eventLock);}
/* Capture and clear only the current exception, preserving exact primary
 * identity. Cleanup can then make JNI calls; secondary failures are retained. */
static void capture(JNIEnv* env,KLHostCallbacks* callbacks,jthrowable* primary){
 jthrowable value=(*env)->ExceptionOccurred(env);if(!value)return;(*env)->ExceptionClear(env);
 if(!*primary){*primary=value;return;}
 if(!(*env)->IsSameObject(env,*primary,value)){
  kl_host_suppressed(env,callbacks,kl_host_bind_KLHost_java_lang_Throwable(env,callbacks,*primary),kl_host_bind_KLHost_java_lang_Throwable(env,callbacks,value));
  if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
 }
 (*env)->DeleteLocalRef(env,value);
}
static void signal_wake(void* owner){
 KLHostScheduler* s=owner;if(atomic_load(&s->stopping))return;
 if(!atomic_exchange(&s->pending,1)){kern_return_t code=semaphore_signal(s->semaphore);if(code!=KERN_SUCCESS)atomic_store(&s->error,1);}
}
/* Java queue wakeups share the existing coalesced SDL wake source. */
void kl_host_scheduler_notify(void){
 pthread_mutex_lock(&eventLock);KLHostScheduler* s=activeScheduler;
 if(s&&s->threadStarted&&s->semaphore!=SEMAPHORE_NULL)signal_wake(s);
 pthread_mutex_unlock(&eventLock);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_FrameWakeup_signal(JNIEnv* env,jclass type,jlong owner){(void)env;(void)type;signal_wake((KLHostScheduler*)(uintptr_t)owner);}
static int demand(JNIEnv*,KLHostScheduler*);
static void* dispatch_wakes(void* owner){
 KLHostScheduler* s=owner;JNIEnv* env=NULL;
 if((*s->vm)->AttachCurrentThreadAsDaemon(s->vm,(void**)&env,NULL)!=JNI_OK){atomic_store(&s->error,4);atomic_store(&s->exited,1);return NULL;}
 uint64_t cursor=0;for(;;){kern_return_t code=semaphore_wait(s->semaphore);if(atomic_load(&s->stopping))break;if(code==KERN_ABORTED)continue;if(code!=KERN_SUCCESS){atomic_store(&s->error,2);break;}
  atomic_store(&s->pending,0);
  /* A resumed engine can await a frame synchronously on the UI owner. An SDL
   * wake cannot restart its clock until that callback returns; this dispatcher
   * therefore reconciles actual demand before reading the display mailbox. */
  if(!atomic_load(&s->error))(void)demand(env,s);
  if((*env)->ExceptionCheck(env)){jthrowable error=(*env)->ExceptionOccurred(env);(*env)->ExceptionClear(env);s->dispatcherFailure=(*env)->NewGlobalRef(env,error);(*env)->DeleteLocalRef(env,error);atomic_store(&s->error,4);}
  /* Both Android receiver Loopers and native frame consumers must keep their
   * display transport while Activity.onPause synchronously waits on a render
   * thread. Only queue delivery occurs here; callbacks keep their own Looper. */
  KLDisplayTick tick;if(!atomic_load(&s->error)&&kl_display_clock_sample(s->clock,&cursor,&tick)){
   klj_ndk_choreographer_tick(tick.sequence,tick.callbackTimeNanos,tick.predictedOutputTimeNanos,tick.intervalNanos);
   kl_host_dispatch_frame(env,s->callbacks,(jlong)tick.sequence,(jlong)tick.callbackTimeNanos,(jlong)tick.predictedOutputTimeNanos,(jlong)tick.intervalNanos,(jlong)tick.displayId);
   if((*env)->ExceptionCheck(env)){jthrowable error=(*env)->ExceptionOccurred(env);(*env)->ExceptionClear(env);s->dispatcherFailure=(*env)->NewGlobalRef(env,error);(*env)->DeleteLocalRef(env,error);atomic_store(&s->error,4);}
  }
  if(atomic_exchange(&s->eventPending,1)){if(atomic_load(&s->error))break;continue;}
  SDL_Event event={0};event.type=s->eventType;event.user.code=1;
  if(!SDL_PushEvent(&event)){atomic_store(&s->error,3);break;}
  if(atomic_load(&s->error))break;
 }
 (*s->vm)->DetachCurrentThread(s->vm);atomic_store(&s->exited,1);return NULL;
}
KLHostScheduler* kl_host_scheduler_create(JNIEnv* env,KLHostCallbacks* callbacks){
 if(!callbacks||(*env)->ExceptionCheck(env))return NULL;
 KLHostScheduler* s=calloc(1,sizeof(*s));if(!s){fail(env,"Android display scheduler allocation failed");return NULL;}s->owner=env;s->callbacks=callbacks;s->semaphore=SEMAPHORE_NULL;
 if(pthread_mutex_init(&s->sourceLock,NULL)){free(s);fail(env,"Android display source lock could not be created");return NULL;}
 if((*env)->GetJavaVM(env,&s->vm)!=JNI_OK){pthread_mutex_destroy(&s->sourceLock);free(s);fail(env,"Android display scheduler could not acquire the Java VM");return NULL;}
 pthread_mutex_lock(&eventLock);if(activeScheduler){pthread_mutex_unlock(&eventLock);pthread_mutex_destroy(&s->sourceLock);free(s);fail(env,"Another Android display scheduler is active");return NULL;}activeScheduler=s;pthread_mutex_unlock(&eventLock);
 return s;
}
int kl_host_scheduler_window(JNIEnv* env,KLHostScheduler* s,void* window){
 if(!owns(env,s))return 0;if(s->clock||s->stopped){fail(env,"Android display scheduler window is already assigned");return 0;}
 char error[256]={0};s->clock=kl_display_clock_create(window,error,sizeof(error));if(!s->clock){fail(env,error);return 0;}
 if(!acquire_event(env,s))return 0;
 if(semaphore_create(mach_task_self(),&s->semaphore,SYNC_POLICY_FIFO,0)!=KERN_SUCCESS){fail(env,"Android display wake semaphore creation failed");return 0;}
 if(!kl_display_clock_set_wake(s->clock,signal_wake,s,error,sizeof(error))){fail(env,error);return 0;}
 if(pthread_create(&s->dispatcher,NULL,dispatch_wakes,s)!=0){fail(env,"Android display wake dispatcher creation failed");return 0;}s->threadStarted=1;
 klj_ndk_choreographer_set_wakeup(signal_wake,s);
 KLHost_maclauncher_android_bridge_FrameWakeup callback=kl_host_wakeup_create(env,s->callbacks,(jlong)(uintptr_t)s);
 if(!callback.object)return 0;kl_host_display_wakeup(env,s->callbacks,kl_host_bind_KLHost_java_lang_Runnable(env,s->callbacks,callback.object));(*env)->DeleteLocalRef(env,callback.object);return !(*env)->ExceptionCheck(env);
}
int kl_host_scheduler_changed_display(JNIEnv* env,KLHostScheduler* s,void* window){if(!owns(env,s))return 0;char error[256]={0};if(!kl_display_clock_update_display(s->clock,window,error,sizeof(error))){fail(env,error);return 0;}return 1;}
int kl_host_scheduler_event(KLHostScheduler* s,const SDL_Event* event){if(!s||!s->eventType||event->type!=s->eventType)return 0;atomic_store(&s->eventPending,0);return 1;}
static int demand(JNIEnv* env,KLHostScheduler* s){
 jboolean needed=kl_host_needs_frames(env,s->callbacks);if((*env)->ExceptionCheck(env))return -1;
 needed=needed||klj_ndk_choreographer_needs_frames();
 jboolean active=kl_host_display_inflight(env,s->callbacks);if((*env)->ExceptionCheck(env))return -1;
 active=active||klj_ndk_choreographer_inflight();
 char error[256]={0};pthread_mutex_lock(&s->sourceLock);
 if(atomic_load(&s->stopping)){pthread_mutex_unlock(&s->sourceLock);return 0;}
 needed=needed||active||atomic_load(&s->forceFrames);
 int success=kl_display_clock_set_demand(s->clock,needed,error,sizeof(error));pthread_mutex_unlock(&s->sourceLock);
 if(!success){fail(env,error);return -1;}return needed;
}
static int dispatcher_ok(JNIEnv* env,KLHostScheduler* s){if(!atomic_load(&s->error))return 1;if(s->dispatcherFailure)(*env)->Throw(env,s->dispatcherFailure);else fail(env,"Android display wake dispatcher failed");return 0;}
int kl_host_scheduler_pump(JNIEnv* env,KLHostScheduler* s){if(!owns(env,s)||s->stopped||!dispatcher_ok(env,s))return 0;KLDisplayTick tick;if(kl_display_clock_poll(s->clock,&tick)){kl_host_present_frame(env,s->callbacks);if((*env)->ExceptionCheck(env))return 0;}return 1;}
int kl_host_scheduler_wait(JNIEnv* env,KLHostScheduler* s,int existingWait){if(!owns(env,s)||s->stopped||!dispatcher_ok(env,s))return -1;jint value=kl_host_display_wait(env,s->callbacks,klj_ndk_choreographer_wait_millis(existingWait));return (*env)->ExceptionCheck(env)?-1:value;}
int kl_host_scheduler_begin_close(JNIEnv* env,KLHostScheduler* s){if(!owns(env,s))return 0;if(!s->clock||s->stopped)return 1;atomic_store(&s->forceFrames,1);signal_wake(s);return 1;}
int kl_host_scheduler_quiesce_pump(JNIEnv* env,KLHostScheduler* s,unsigned timeoutMillis,KLHostTaskPump pump,void* context){
 if(!owns(env,s))return 0;if(s->retired)return 1;jthrowable primary=NULL;capture(env,s->callbacks,&primary);
 int stopped=1,javaRetired=1,drained=0;char error[256]={0};pthread_mutex_lock(&s->sourceLock);atomic_store(&s->stopping,1);if(s->clock)stopped=kl_display_clock_set_demand(s->clock,0,error,sizeof(error));pthread_mutex_unlock(&s->sourceLock);
 if(!stopped){fail(env,error);capture(env,s->callbacks,&primary);}
 // No realtime producer remains after successful Stop. Java clears its wake
 // under the same lock used to signal, so a late registration cannot use s.
 s->stopped=1;klj_ndk_choreographer_detach();kl_host_display_shutdown(env,s->callbacks);if((*env)->ExceptionCheck(env)){javaRetired=0;capture(env,s->callbacks,&primary);}
 atomic_store(&s->stopping,1);if(s->threadStarted&&semaphore_signal(s->semaphore)!=KERN_SUCCESS){fail(env,"Android display dispatcher could not be stopped");capture(env,s->callbacks,&primary);}
 uint64_t began=kl_display_clock_monotonic_now(),budget=(uint64_t)timeoutMillis*1000000;
 for(;;){jboolean active=0;if(javaRetired){active=kl_host_display_inflight(env,s->callbacks);if((*env)->ExceptionCheck(env)){javaRetired=0;capture(env,s->callbacks,&primary);}}
  active=active||klj_ndk_choreographer_inflight();
  if((!javaRetired||!active)&&(!s->threadStarted||atomic_load(&s->exited))){drained=javaRetired;break;}
  if(kl_display_clock_monotonic_now()-began>=budget){fail(env,"Android frame callback or dispatcher remained active during window shutdown");capture(env,s->callbacks,&primary);break;}
  if(pump){pump(env,context);capture(env,s->callbacks,&primary);}sleep_one_ms();}
 if(s->threadStarted&&atomic_load(&s->exited)){if(pthread_join(s->dispatcher,NULL)){fail(env,"Android display dispatcher join failed");capture(env,s->callbacks,&primary);}else s->threadStarted=0;}
 /* Dispatcher can no longer enqueue. No owner pointer ever entered SDL's
  * queue; flush only the reserved event, preserving all unrelated events. */
 if(!s->threadStarted&&stopped&&javaRetired&&drained){release_event(s);s->retired=1;}
 if(primary){(*env)->Throw(env,primary);(*env)->DeleteLocalRef(env,primary);}return s->retired;
}
int kl_host_scheduler_quiesce(JNIEnv* env,KLHostScheduler* s,unsigned timeoutMillis){return kl_host_scheduler_quiesce_pump(env,s,timeoutMillis,NULL,NULL);}
int kl_host_scheduler_destroy(JNIEnv* env,KLHostScheduler* s){if(!owns(env,s))return 0;if(!s->retired){fail(env,"Android display scheduler has not retired its callbacks");return 0;}char error[256]={0};if(s->clock&&!kl_display_clock_destroy(s->clock,error,sizeof(error))){fail(env,error);return 0;}s->clock=NULL;
 if(s->semaphore!=SEMAPHORE_NULL&&semaphore_destroy(mach_task_self(),s->semaphore)!=KERN_SUCCESS){fail(env,"Android display wake semaphore retirement failed");return 0;}
 if(s->dispatcherFailure)(*env)->DeleteGlobalRef(env,s->dispatcherFailure);release_scheduler(s);pthread_mutex_destroy(&s->sourceLock);free(s);return 1;
}
