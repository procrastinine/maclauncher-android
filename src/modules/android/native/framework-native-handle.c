#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>

/* Native records integrate platform owners with the original AOSP
 * allocation registry. No Java map retains its owning framework object. */
struct Handle { jobject state; uint64_t identity; unsigned references; struct Handle *next; };
static struct Handle *handles;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
// This counter shares the registry lock. Zero remains retired forever after
// wraparound, so even exhausted identity space cannot revive an old owner.
static uint64_t next_identity=1;
static JavaVM *vm;
static jmethodID close_method;
static struct Handle *find(jlong value){for(struct Handle *item=handles;item;item=item->next)if(item->identity==(uint64_t)value)return item;return NULL;}
static void invalid(JNIEnv *env){jclass error=(*env)->FindClass(env,"java/lang/IllegalStateException");if(error){(*env)->ThrowNew(env,error,"Platform handle has closed");(*env)->DeleteLocalRef(env,error);}}
static int retain(void *pointer){
    pthread_mutex_lock(&lock);struct Handle *item=find((jlong)(uintptr_t)pointer);int valid=item&&item->references<UINT_MAX;if(valid)item->references++;pthread_mutex_unlock(&lock);return valid;
}
static void retire(void *pointer){
    jobject state=NULL;JavaVM *machine=NULL;jmethodID close=NULL;pthread_mutex_lock(&lock);
    uint64_t identity=(uint64_t)(uintptr_t)pointer;
    struct Handle **slot=&handles;while(*slot&&(*slot)->identity!=identity)slot=&(*slot)->next;
    if(*slot){struct Handle *item=*slot;if(!--item->references){*slot=item->next;state=item->state;machine=vm;close=close_method;free(item);}}
    pthread_mutex_unlock(&lock);
    if(!state)return;
    JNIEnv *env=NULL;int attached=0;
    if((*machine)->GetEnv(machine,(void**)&env,JNI_VERSION_1_6)!=JNI_OK){if((*machine)->AttachCurrentThreadAsDaemon(machine,(void**)&env,NULL)!=JNI_OK)return;attached=1;}
    jthrowable pending=(*env)->ExceptionOccurred(env);if(pending)(*env)->ExceptionClear(env);
    (*env)->CallVoidMethod(env,state,close);(*env)->DeleteGlobalRef(env,state);
    if(pending){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,pending);(*env)->DeleteLocalRef(env,pending);}
    if(attached){if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);}(*machine)->DetachCurrentThread(machine);}
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostNativeHandle_create(JNIEnv *env,jclass owner,jobject state){
    (void)owner;if(!state){invalid(env);return 0;}
    struct Handle *item=calloc(1,sizeof(*item));if(!item){jclass error=(*env)->FindClass(env,"java/lang/OutOfMemoryError");if(error){(*env)->ThrowNew(env,error,"Platform handle allocation failed");(*env)->DeleteLocalRef(env,error);}return 0;}
    item->state=(*env)->NewGlobalRef(env,state);if(!item->state){free(item);return 0;}item->references=1;
    jclass closeable=(*env)->FindClass(env,"java/lang/AutoCloseable");jmethodID method=closeable?(*env)->GetMethodID(env,closeable,"close","()V"):NULL;if(closeable)(*env)->DeleteLocalRef(env,closeable);
    if(!method){(*env)->DeleteGlobalRef(env,item->state);free(item);return 0;}
    JavaVM *current=NULL;(*env)->GetJavaVM(env,&current);
    pthread_mutex_lock(&lock);
    if(!next_identity){pthread_mutex_unlock(&lock);(*env)->DeleteGlobalRef(env,item->state);free(item);jclass error=(*env)->FindClass(env,"java/lang/OutOfMemoryError");if(error){(*env)->ThrowNew(env,error,"Platform handle identities are exhausted");(*env)->DeleteLocalRef(env,error);}return 0;}
    vm=current;close_method=method;item->identity=next_identity++;item->next=handles;handles=item;
    jlong identity=(jlong)item->identity;pthread_mutex_unlock(&lock);return identity;
}
JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostNativeHandle_state(JNIEnv *env,jclass owner,jlong value){
    (void)owner;pthread_mutex_lock(&lock);struct Handle *item=find(value);jobject result=item?(*env)->NewLocalRef(env,item->state):NULL;pthread_mutex_unlock(&lock);if(!item)invalid(env);return result;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostNativeHandle_retain(JNIEnv *env,jclass owner,jlong value){
    (void)owner;if(!retain((void*)(uintptr_t)value)){invalid(env);return 0;}return value;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostNativeHandle_release(JNIEnv *env,jclass owner,jlong value){(void)env;(void)owner;retire((void*)(uintptr_t)value);}
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostNativeHandle_finalizer(JNIEnv *env,jclass owner){(void)env;(void)owner;return (jlong)(uintptr_t)&retire;}
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostNativeHandle_retainer(JNIEnv *env,jclass owner){(void)env;(void)owner;return (jlong)(uintptr_t)&retain;}
