#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

/* The original Java MessageQueue shares Klepton's actual thread-local ALooper.
 * Handles are registry IDs, so a retired FD snapshot cannot dereference freed
 * Java callback storage. No Java message list exists in this adapter. */
typedef int (*callback_fn)(int,int,void*);
typedef struct {
    void *(*prepare)(int); void (*acquire)(void*),(*release)(void*),(*wake)(void*);
    int (*poll)(int,int*,int*,void**),(*add)(void*,int,int,int,callback_fn,void*),(*remove)(void*,int);
} looper_api;
static looper_api api;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
typedef struct queue {
    uint64_t handle; unsigned refs; int closed;
    void *looper; pthread_t thread; JavaVM *vm; jweak object; jmethodID dispatch;
    pthread_mutex_t lock; struct {int fd,events;uint64_t sequence;} *descriptors;size_t count,capacity;uint64_t sequence;
    struct queue *next;
    atomic_int polling,main;
} queue;
static queue *queues;
static uint64_t serial;
static void fail(JNIEnv *env,const char *type,const char *message){if((*env)->ExceptionCheck(env))return;jclass c=(*env)->FindClass(env,type);if(c){(*env)->ThrowNew(env,c,message);(*env)->DeleteLocalRef(env,c);}}
static int ready(JNIEnv *env){
    pthread_mutex_lock(&gate);
    if(!api.prepare){void *(*lookup)(const char*)=dlsym(RTLD_DEFAULT,"kl_ndk_lookup");if(lookup){
        api.prepare=lookup("ALooper_prepare");api.acquire=lookup("ALooper_acquire");api.release=lookup("ALooper_release");api.wake=lookup("ALooper_wake");api.poll=lookup("ALooper_pollOnce");api.add=lookup("ALooper_addFd");api.remove=lookup("ALooper_removeFd");}}
    int result=api.prepare&&api.acquire&&api.release&&api.wake&&api.poll&&api.add&&api.remove;
    pthread_mutex_unlock(&gate);if(!result)fail(env,"java/lang/UnsatisfiedLinkError","Klepton message poller is not loaded");return result;
}
static queue *retain(uint64_t handle){
    pthread_mutex_lock(&gate);queue *q=queues;while(q&&q->handle!=handle)q=q->next;if(q&&!q->closed)q->refs++;else q=NULL;pthread_mutex_unlock(&gate);return q;
}
static void release(JNIEnv *env,queue *q){
    pthread_mutex_lock(&gate);int last=--q->refs==0;pthread_mutex_unlock(&gate);
    if(last){if(q->object)(*env)->DeleteWeakGlobalRef(env,q->object);api.release(q->looper);pthread_mutex_destroy(&q->lock);free(q->descriptors);free(q);}
}
static void host_wake(queue *q){
    if(!atomic_load(&q->main))return;
    typedef void(*wake_fn)(void);static _Atomic(wake_fn) function;
    wake_fn current=atomic_load(&function);if(!current){current=(wake_fn)dlsym(RTLD_DEFAULT,"kl_host_scheduler_notify");if(current)atomic_store(&function,current);}if(current)current();
}
static int events_callback(int fd,int events,void *data){
    queue *q=retain((uint64_t)(uintptr_t)data);if(!q)return 0;
    JNIEnv *env=NULL;int attached=0;
    if((*q->vm)->GetEnv(q->vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK){
        if((*q->vm)->AttachCurrentThread(q->vm,(void**)&env,NULL)!=JNI_OK){pthread_mutex_lock(&gate);q->refs--;pthread_mutex_unlock(&gate);return 0;}attached=1;
    }
    pthread_mutex_lock(&q->lock);size_t index=0;while(index<q->count&&q->descriptors[index].fd!=fd)index++;
    uint64_t sequence=index<q->count?q->descriptors[index].sequence:0;
    jobject target=q->object?(*env)->NewLocalRef(env,q->object):NULL;jmethodID dispatch=q->dispatch;pthread_mutex_unlock(&q->lock);
    int next=0;if(sequence&&target&&dispatch&&!(*env)->ExceptionCheck(env)){
        next=(*env)->CallIntMethod(env,target,dispatch,fd,(events&3)|((events&28)?4:0));
        if((*env)->ExceptionCheck(env))next=0;
    }
    if(target)(*env)->DeleteLocalRef(env,target);
    pthread_mutex_lock(&q->lock);index=0;while(index<q->count&&q->descriptors[index].fd!=fd)index++;
    // Java listeners may remove/re-register this descriptor during dispatch.
    // Their newer registration takes precedence over this callback's result.
    if(index<q->count&&q->descriptors[index].sequence==sequence){
        if(next&&next!=q->descriptors[index].events){
            if(api.add(q->looper,fd,-1,next,events_callback,data)<0){fail(env,"java/lang/IllegalStateException","Native file descriptor registration failed");next=0;}
            else{q->descriptors[index].events=next;q->descriptors[index].sequence=++q->sequence;}
        }
        if(!next)q->descriptors[index]=q->descriptors[--q->count];
    }
    pthread_mutex_unlock(&q->lock);JavaVM *vm=q->vm;release(env,q);if(attached)(*vm)->DetachCurrentThread(vm);return next!=0;
}
JNIEXPORT jlong JNICALL Java_android_os_MessageQueue_nativeInit(JNIEnv *env,jclass owner){
    (void)owner;if(!ready(env))return 0;
    queue *q=calloc(1,sizeof(*q));if(!q){fail(env,"java/lang/OutOfMemoryError","Message poller allocation failed");return 0;}
    q->looper=api.prepare(1);if(!q->looper){free(q);fail(env,"java/lang/OutOfMemoryError","Native looper allocation failed");return 0;}
    api.acquire(q->looper);q->thread=pthread_self();(*env)->GetJavaVM(env,&q->vm);pthread_mutex_init(&q->lock,NULL);q->refs=1;
    pthread_mutex_lock(&gate);uint64_t handle=serial==UINT64_MAX?0:++serial;
    if(handle){q->handle=handle;q->next=queues;queues=q;}pthread_mutex_unlock(&gate);
    if(!handle){release(env,q);fail(env,"java/lang/OutOfMemoryError","Android message queue identity space exhausted");return 0;}return (jlong)handle;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostLooper_attachNative(JNIEnv *env,jclass owner,jobject object,jlong handle){
    (void)owner;queue *q=retain((uint64_t)handle);if(!q){fail(env,"java/lang/IllegalStateException","Message queue has been destroyed");return;}
    jclass c=(*env)->GetObjectClass(env,object);jmethodID dispatch=c?(*env)->GetMethodID(env,c,"dispatchEvents","(II)I"):NULL;
    if(c)(*env)->DeleteLocalRef(env,c);if(dispatch){jweak reference=(*env)->NewWeakGlobalRef(env,object);if(reference){pthread_mutex_lock(&q->lock);jweak old=q->object;q->object=reference;q->dispatch=dispatch;if(old)(*env)->DeleteWeakGlobalRef(env,old);pthread_mutex_unlock(&q->lock);}}
    release(env,q);
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostLooper_mainNative(JNIEnv *env,jclass owner,jlong handle){
    (void)owner;queue *q=retain((uint64_t)handle);if(q){atomic_store(&q->main,1);release(env,q);}
}
JNIEXPORT void JNICALL Java_android_os_MessageQueue_nativeDestroy(JNIEnv *env,jclass owner,jlong handle){
    (void)owner;pthread_mutex_lock(&gate);queue **slot=&queues;while(*slot&&(*slot)->handle!=(uint64_t)handle)slot=&(*slot)->next;queue *q=*slot;if(q){*slot=q->next;q->closed=1;}pthread_mutex_unlock(&gate);if(!q)return;
    pthread_mutex_lock(&q->lock);for(size_t i=0;i<q->count;i++)api.remove(q->looper,q->descriptors[i].fd);q->count=0;pthread_mutex_unlock(&q->lock);api.wake(q->looper);release(env,q);
}
static int poll_once(JNIEnv *env,jobject object,jlong handle,jint timeout){
    (void)object;queue *q=retain((uint64_t)handle);if(!q){fail(env,"java/lang/IllegalStateException","Message queue has been destroyed");return -4;}
    if(!pthread_equal(q->thread,pthread_self())){release(env,q);fail(env,"java/lang/IllegalStateException","Message queue polling requires its owner thread");return -4;}
    atomic_store(&q->polling,1);int result=api.poll(timeout,NULL,NULL,NULL);atomic_store(&q->polling,0);release(env,q);
    if(result==-4)fail(env,"java/lang/IllegalStateException","Native message polling failed");return result;
}
JNIEXPORT void JNICALL Java_android_os_MessageQueue_nativePollOnce(JNIEnv *env,jobject object,jlong handle,jint timeout){(void)poll_once(env,object,handle,timeout);}
JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostLooper_pollNative(JNIEnv *env,jclass owner,jobject object,jlong handle,jint timeout){(void)owner;return poll_once(env,object,handle,timeout);}
JNIEXPORT void JNICALL Java_android_os_MessageQueue_nativeWake(JNIEnv *env,jclass owner,jlong handle){
    (void)owner;queue *q=retain((uint64_t)handle);if(q){api.wake(q->looper);host_wake(q);release(env,q);}
}
JNIEXPORT jboolean JNICALL Java_android_os_MessageQueue_nativeIsPolling(JNIEnv *env,jclass owner,jlong handle){
    (void)owner;queue *q=retain((uint64_t)handle);if(!q)return JNI_FALSE;int result=atomic_load(&q->polling);release(env,q);return result?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT void JNICALL Java_android_os_MessageQueue_nativeSetFileDescriptorEvents(JNIEnv *env,jclass owner,jlong handle,jint fd,jint events){
    (void)owner;queue *q=retain((uint64_t)handle);if(!q)return;
    pthread_mutex_lock(&q->lock);size_t index=0;while(index<q->count&&q->descriptors[index].fd!=fd)index++;
    if(events){
        if(index==q->count&&index==q->capacity){
            size_t capacity=q->capacity?q->capacity*2:16;
            void *grown=capacity>q->capacity&&capacity<=SIZE_MAX/sizeof(*q->descriptors)?realloc(q->descriptors,capacity*sizeof(*q->descriptors)):NULL;
            if(!grown){pthread_mutex_unlock(&q->lock);release(env,q);fail(env,"java/lang/OutOfMemoryError","Message descriptor allocation failed");return;}
            q->descriptors=grown;q->capacity=capacity;
        }
        if((events&~7)||fd<0||api.add(q->looper,fd,-1,events,events_callback,(void*)(uintptr_t)q->handle)<0)fail(env,"java/lang/IllegalStateException","Native file descriptor registration failed");
        else{if(index==q->count)q->count++;q->descriptors[index]=(typeof(q->descriptors[0])){fd,events,++q->sequence};}
    }else{api.remove(q->looper,fd);if(index<q->count)q->descriptors[index]=q->descriptors[--q->count];}
    pthread_mutex_unlock(&q->lock);host_wake(q);release(env,q);
}
