#include <jni.h>
#include "native-import-registry.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "host-callbacks.h"

/* AInputQueue is a native platform transport, not a second Android event model.
 * Events retain complete upstream KeyEvent/MotionEvent snapshots. NDK getters
 * read those objects, including original pointer axes and historical samples. */
extern void *klj_guest_runtime_symbol(const char *);
typedef int (*looper_callback)(int,int,void*);
typedef struct event event;
typedef struct queue {
    jobject owner; KLHostCallbacks *bindings; JavaVM *vm; int pipes[2],closed;unsigned refs,count;
    void *looper;event *first,*last;struct queue *registry_next;
} queue;
struct event {queue *queue;jobject object;uint64_t id;unsigned refs;int type,delivered,pre_dispatch;event *next,*registry_next;};
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static queue *queues;static event *events;static uint64_t serial;
static JavaVM *java_vm;
static int (*add_fd)(void*,int,int,int,looper_callback,void*),(*remove_fd)(void*,int);
static void (*acquire_looper)(void*),(*release_looper)(void*);
static int installed;
static void fail(JNIEnv *env,const char *message){if((*env)->ExceptionCheck(env))return;jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");if(type){(*env)->ThrowNew(env,type,message);(*env)->DeleteLocalRef(env,type);}}
static queue *find_queue(void *pointer){for(queue *q=queues;q;q=q->registry_next)if(q==pointer)return q;return NULL;}
static event *find_event(const void *pointer){for(event *e=events;e;e=e->registry_next)if(e==pointer)return e;return NULL;}
static JNIEnv *environment(int *attached){JNIEnv *env=NULL;*attached=0;if(!java_vm)return NULL;jint status=(*java_vm)->GetEnv(java_vm,(void**)&env,JNI_VERSION_1_6);if(status==JNI_EDETACHED){if((*java_vm)->AttachCurrentThreadAsDaemon(java_vm,(void**)&env,NULL)!=JNI_OK)return NULL;*attached=1;}else if(status!=JNI_OK)return NULL;return env;}
static void release_environment(JNIEnv *env,int attached){if(attached){if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);}(*java_vm)->DetachCurrentThread(java_vm);}}
static void release_queue(JNIEnv *env,queue *q){pthread_mutex_lock(&gate);int last=--q->refs==0;pthread_mutex_unlock(&gate);if(last){(*env)->DeleteGlobalRef(env,q->owner);kl_host_callbacks_release(env,q->bindings);free(q);}}
static void free_event(JNIEnv *env,event *e){pthread_mutex_lock(&gate);int last=--e->refs==0;pthread_mutex_unlock(&gate);if(!last)return;jthrowable prior=(*env)->ExceptionOccurred(env);if(prior)(*env)->ExceptionClear(env);if(e->type==2){kl_host_queue_motion_recycle(env,e->queue->bindings,kl_host_bind_KLHost_android_view_MotionEvent(env,e->queue->bindings,e->object));(*env)->ExceptionCheck(env);}(*env)->DeleteGlobalRef(env,e->object);queue *q=e->queue;free(e);release_queue(env,q);if(prior){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,prior);(*env)->DeleteLocalRef(env,prior);}}
static void attach_looper(queue *pointer,void *looper,int ident,looper_callback callback,void *data){
    pthread_mutex_lock(&gate);queue *q=find_queue(pointer);if(q&&!q->closed){if(q->looper){remove_fd(q->looper,q->pipes[0]);release_looper(q->looper);}q->looper=looper;if(looper){acquire_looper(looper);add_fd(looper,q->pipes[0],ident,1,callback,data);}}pthread_mutex_unlock(&gate);
}
static void detach_looper(queue *q){attach_looper(q,NULL,0,NULL,NULL);}
static int has_events(queue *pointer){pthread_mutex_lock(&gate);queue *q=find_queue(pointer);int result=q&&q->first!=NULL;pthread_mutex_unlock(&gate);return result;}
static int get_event(queue *pointer,event **out){
    if(!out)return -EINVAL;*out=NULL;pthread_mutex_lock(&gate);queue *q=find_queue(pointer);
    if(!q||q->closed){pthread_mutex_unlock(&gate);return -EINVAL;}event *e=q->first;
    if(!e){pthread_mutex_unlock(&gate);return -EAGAIN;}q->first=e->next;if(!q->first)q->last=NULL;e->next=NULL;e->delivered=1;
    if(!q->first){char signal;ssize_t result;do{result=read(q->pipes[0],&signal,1);}while(result<0&&errno==EINTR);}*out=e;pthread_mutex_unlock(&gate);return 0;
}
static void finish_event(queue*,event*,int);
static int pre_dispatch(queue *pointer,event *value){pthread_mutex_lock(&gate);event *e=find_event(value);int pre=e&&e->queue==pointer&&e->delivered&&e->pre_dispatch;pthread_mutex_unlock(&gate);if(pre)finish_event(pointer,value,0);return pre;/* Return pre-IME events to the original ViewRoot IME stages. */}
static void finish_event(queue *pointer,event *value,int handled){
    int attached;JNIEnv *env=environment(&attached);if(!env)return;
    pthread_mutex_lock(&gate);event *e=find_event(value);queue *q=find_queue(pointer);
    if(!e||!q||e->queue!=q||!e->delivered){pthread_mutex_unlock(&gate);release_environment(env,attached);return;}
    event **slot=&events;while(*slot&&*slot!=e)slot=&(*slot)->registry_next;if(*slot)*slot=e->registry_next;q->count--;jobject owner=(*env)->NewLocalRef(env,q->owner);pthread_mutex_unlock(&gate);
    if(owner){kl_host_queue_finish(env,q->bindings,kl_host_bind_KLHost_maclauncher_android_framework_HostInputQueue_Owner(env,q->bindings,owner),(jlong)e->id,(jboolean)(handled!=0));(*env)->ExceptionCheck(env);(*env)->DeleteLocalRef(env,owner);}free_event(env,e);release_environment(env,attached);
}
typedef struct {JNIEnv *env;jobject object;KLHostCallbacks *bindings;event *entry;int attached,type;} event_access;
static event_access get_access(const event *pointer){event_access result={0};result.env=environment(&result.attached);if(!result.env)return result;pthread_mutex_lock(&gate);event *e=find_event(pointer);if(e){e->refs++;result.entry=e;result.type=e->type;result.object=e->object;result.bindings=e->queue->bindings;}pthread_mutex_unlock(&gate);return result;}
static void end_access(event_access *a){if(a->env)(*a->env)->ExceptionCheck(a->env);if(a->entry)free_event(a->env,a->entry);if(a->env)release_environment(a->env,a->attached);}
/* Each NDK getter consumes the full upstream declaration through generated bindings.
 * The access keeps its event, queue and binding table alive through concurrent disposal. */
static int event_type(const event *event){event_access a=get_access(event);int type=a.type;end_access(&a);return type;}
static int input_getDeviceId(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_input_getdeviceid(a.env,a.bindings,kl_host_bind_KLHost_android_view_InputEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int input_getSource(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_input_getsource(a.env,a.bindings,kl_host_bind_KLHost_android_view_InputEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int key_getAction(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_key_getaction(a.env,a.bindings,kl_host_bind_KLHost_android_view_KeyEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int key_getFlags(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_key_getflags(a.env,a.bindings,kl_host_bind_KLHost_android_view_KeyEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int key_getKeyCode(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_key_getkeycode(a.env,a.bindings,kl_host_bind_KLHost_android_view_KeyEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int key_getScanCode(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_key_getscancode(a.env,a.bindings,kl_host_bind_KLHost_android_view_KeyEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int key_getMetaState(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_key_getmetastate(a.env,a.bindings,kl_host_bind_KLHost_android_view_KeyEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int key_getRepeatCount(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_key_getrepeatcount(a.env,a.bindings,kl_host_bind_KLHost_android_view_KeyEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int64_t key_getDownTime(const event *e){event_access a=get_access(e);int64_t value=0;if(a.object)value=(int64_t)kl_host_queue_key_getdowntime(a.env,a.bindings,kl_host_bind_KLHost_android_view_KeyEvent(a.env,a.bindings,a.object))*1000000;end_access(&a);return value;}
static int64_t key_getEventTime(const event *e){event_access a=get_access(e);int64_t value=0;if(a.object)value=(int64_t)kl_host_queue_key_geteventtime(a.env,a.bindings,kl_host_bind_KLHost_android_view_KeyEvent(a.env,a.bindings,a.object))*1000000;end_access(&a);return value;}
static int motion_getAction(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_motion_getaction(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int motion_getFlags(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_motion_getflags(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int motion_getMetaState(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_motion_getmetastate(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int motion_getButtonState(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_motion_getbuttonstate(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int motion_getEdgeFlags(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_motion_getedgeflags(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int motion_getClassification(const event *e){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_motion_getclassification(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int64_t motion_getDownTime(const event *e){event_access a=get_access(e);int64_t value=0;if(a.object)value=(int64_t)kl_host_queue_motion_getdowntime(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object))*1000000;end_access(&a);return value;}
static int64_t motion_getEventTimeNanos(const event *e){event_access a=get_access(e);int64_t value=0;if(a.object)value=(int64_t)kl_host_queue_motion_geteventtimenanos(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static float motion_getXPrecision(const event *e){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getxprecision(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static float motion_getYPrecision(const event *e){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getyprecision(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static size_t motion_getPointerCount(const event *e){event_access a=get_access(e);size_t value=0;if(a.object)value=(size_t)kl_host_queue_motion_getpointercount(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static size_t motion_getHistorySize(const event *e){event_access a=get_access(e);size_t value=0;if(a.object)value=(size_t)kl_host_queue_motion_gethistorysize(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object));end_access(&a);return value;}
static int motion_getPointerId(const event *e,size_t index){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_motion_getpointerid(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static int motion_getToolType(const event *e,size_t index){event_access a=get_access(e);int value=0;if(a.object)value=(int)kl_host_queue_motion_gettooltype(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getX(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getx(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getY(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gety(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getRawX(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getrawx(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getRawY(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getrawy(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getPressure(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getpressure(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getSize(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getsize(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getTouchMajor(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gettouchmajor(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getTouchMinor(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gettouchminor(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getToolMajor(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gettoolmajor(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getToolMinor(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gettoolminor(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getOrientation(const event *e,size_t index){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getorientation(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)index);end_access(&a);return value;}
static float motion_getHistoricalX(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricalx(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getHistoricalY(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricaly(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getHistoricalPressure(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricalpressure(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getHistoricalSize(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricalsize(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getHistoricalTouchMajor(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricaltouchmajor(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getHistoricalTouchMinor(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricaltouchminor(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getHistoricalToolMajor(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricaltoolmajor(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getHistoricalToolMinor(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricaltoolminor(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getHistoricalOrientation(const event *e,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricalorientation(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)pointer,(jint)history);end_access(&a);return value;}
static float motion_getAxisValue(const event *e,int axis,size_t pointer){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_getaxisvalue(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),axis,(jint)pointer);end_access(&a);return value;}
static float motion_getHistoricalAxisValue(const event *e,int axis,size_t pointer,size_t history){event_access a=get_access(e);float value=0;if(a.object)value=(float)kl_host_queue_motion_gethistoricalaxisvalue(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),axis,(jint)pointer,(jint)history);end_access(&a);return value;}
static int64_t motion_getHistoricalEventTimeNanos(const event *e,size_t history){event_access a=get_access(e);int64_t value=0;if(a.object)value=(int64_t)kl_host_queue_motion_gethistoricaleventtimenanos(a.env,a.bindings,kl_host_bind_KLHost_android_view_MotionEvent(a.env,a.bindings,a.object),(jint)history);end_access(&a);return value;}
static int64_t motion_getEventTime(const event *e){return motion_getEventTimeNanos(e);}
static int64_t motion_getHistoricalEventTime(const event *e,size_t history){return motion_getHistoricalEventTimeNanos(e,history);}
static float motion_getXOffset(const event *e){return motion_getX(e,0)-motion_getRawX(e,0);}
static float motion_getYOffset(const event *e){return motion_getY(e,0)-motion_getRawY(e,0);}
static void *lookup(const char *name){
#define ENTRY(symbol,fn) if(!strcmp(name,symbol))return(void*)(fn)
#define INPUT(name) ENTRY("AInputEvent_"#name,input_##name)
#define KEY(name) ENTRY("AKeyEvent_"#name,key_##name)
#define MOTION(name) ENTRY("AMotionEvent_"#name,motion_##name)
    ENTRY("AInputQueue_attachLooper",attach_looper);ENTRY("AInputQueue_detachLooper",detach_looper);ENTRY("AInputQueue_hasEvents",has_events);ENTRY("AInputQueue_getEvent",get_event);ENTRY("AInputQueue_preDispatchEvent",pre_dispatch);ENTRY("AInputQueue_finishEvent",finish_event);ENTRY("AInputEvent_getType",event_type);
    INPUT(getDeviceId);INPUT(getSource);KEY(getAction);KEY(getFlags);KEY(getKeyCode);KEY(getScanCode);KEY(getMetaState);KEY(getRepeatCount);KEY(getDownTime);KEY(getEventTime);
    MOTION(getAction);MOTION(getFlags);MOTION(getMetaState);MOTION(getButtonState);MOTION(getEdgeFlags);MOTION(getDownTime);MOTION(getEventTime);MOTION(getXPrecision);MOTION(getYPrecision);MOTION(getPointerCount);MOTION(getPointerId);MOTION(getToolType);MOTION(getX);MOTION(getY);MOTION(getRawX);MOTION(getRawY);MOTION(getPressure);MOTION(getSize);MOTION(getTouchMajor);MOTION(getTouchMinor);MOTION(getToolMajor);MOTION(getToolMinor);MOTION(getOrientation);MOTION(getAxisValue);MOTION(getXOffset);MOTION(getYOffset);MOTION(getHistorySize);MOTION(getHistoricalEventTime);MOTION(getHistoricalX);MOTION(getHistoricalY);MOTION(getHistoricalPressure);MOTION(getHistoricalSize);MOTION(getHistoricalTouchMajor);MOTION(getHistoricalTouchMinor);MOTION(getHistoricalToolMajor);MOTION(getHistoricalToolMinor);MOTION(getHistoricalOrientation);MOTION(getHistoricalAxisValue);MOTION(getClassification);
    return NULL;
}
void klj_input_queue_install(JNIEnv *env){
    if(installed)return;void *(*ndk)(const char*)=klj_guest_runtime_symbol("kl_ndk_lookup");int (*install)(KljImportResolver)=klj_guest_runtime_symbol("klj_register_import_resolver");
    if(!ndk||!install){fail(env,"Klepton input queue transport is unavailable");return;}
    add_fd=ndk("ALooper_addFd");remove_fd=ndk("ALooper_removeFd");acquire_looper=ndk("ALooper_acquire");release_looper=ndk("ALooper_release");
    if(!add_fd||!remove_fd||!acquire_looper||!release_looper){fail(env,"Klepton input queue poller is unavailable");return;}
    if(!install(lookup)){fail(env,"Native import registration allocation failed");return;}installed=1;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostInputQueue_create(JNIEnv *env,jclass type,jobject owner){
    klj_input_queue_install(env);if((*env)->ExceptionCheck(env))return 0;
    if(!java_vm&&(*env)->GetJavaVM(env,&java_vm)!=JNI_OK){fail(env,"Native input queue has no Java VM");return 0;}
    queue *q=calloc(1,sizeof(*q));if(!q){fail(env,"Native input queue allocation failed");return 0;}q->refs=1;q->vm=java_vm;q->bindings=kl_host_callbacks_create(env);if(!q->bindings){free(q);return 0;}
    if(pipe(q->pipes)){kl_host_callbacks_release(env,q->bindings);free(q);fail(env,"Native input queue pipe failed");return 0;}for(int i=0;i<2;i++){fcntl(q->pipes[i],F_SETFL,fcntl(q->pipes[i],F_GETFL)|O_NONBLOCK);fcntl(q->pipes[i],F_SETFD,FD_CLOEXEC);}
    KLHost_maclauncher_android_framework_HostInputQueue_Owner bound=kl_host_bind_KLHost_maclauncher_android_framework_HostInputQueue_Owner(env,q->bindings,owner);if(bound.object)q->owner=(*env)->NewGlobalRef(env,bound.object);pthread_mutex_lock(&gate);if(q->owner){q->registry_next=queues;queues=q;}pthread_mutex_unlock(&gate);
    if(!q->owner){close(q->pipes[0]);close(q->pipes[1]);release_queue(env,q);return 0;}return(jlong)(uintptr_t)q;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostInputQueue_send(JNIEnv *env,jclass type,jlong handle,jobject object,jint event_type,jboolean pre){
    event *e=calloc(1,sizeof(*e));if(!e){fail(env,"Native input event allocation failed");return 0;}e->object=(*env)->NewGlobalRef(env,object);if(!e->object){free(e);return 0;}e->type=event_type;e->pre_dispatch=pre;e->refs=1;
    pthread_mutex_lock(&gate);queue *q=find_queue((void*)(uintptr_t)handle);
    if(!q||q->closed){pthread_mutex_unlock(&gate);(*env)->DeleteGlobalRef(env,e->object);free(e);fail(env,"Native input queue is closed");return 0;}
    e->queue=q;q->refs++;q->count++;e->id=++serial;e->registry_next=events;events=e;int wake=q->first==NULL;if(q->last)q->last->next=e;else q->first=e;q->last=e;
    if(wake){char signal=1;ssize_t written;do{written=write(q->pipes[1],&signal,1);}while(written<0&&errno==EINTR);}uint64_t id=e->id;pthread_mutex_unlock(&gate);return(jlong)id;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostInputQueue_dispose(JNIEnv *env,jclass type,jlong handle){
    event *retired=NULL;pthread_mutex_lock(&gate);queue *q=find_queue((void*)(uintptr_t)handle);if(!q){pthread_mutex_unlock(&gate);return;}q->closed=1;
    if(q->looper){remove_fd(q->looper,q->pipes[0]);release_looper(q->looper);q->looper=NULL;}queue **slot=&queues;while(*slot&&*slot!=q)slot=&(*slot)->registry_next;if(*slot)*slot=q->registry_next;
    event **pending=&events;while(*pending){event *e=*pending;if(e->queue==q){*pending=e->registry_next;e->registry_next=retired;retired=e;}else pending=&e->registry_next;}q->first=q->last=NULL;q->count=0;close(q->pipes[0]);close(q->pipes[1]);pthread_mutex_unlock(&gate);
    kl_host_queue_close(env,q->bindings,kl_host_bind_KLHost_maclauncher_android_framework_HostInputQueue_Owner(env,q->bindings,q->owner));(*env)->ExceptionCheck(env);while(retired){event *next=retired->registry_next;free_event(env,retired);retired=next;}release_queue(env,q);
}
