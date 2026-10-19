#include <jni.h>
#include "connectivity-core.h"
static JavaVM *vm;
static jclass owner;
static jmethodID changed;
static jthrowable pending_failure;
static void unavailable(JNIEnv *env){jclass error=(*env)->FindClass(env,"java/lang/IllegalStateException");if(error){(*env)->ThrowNew(env,error,"Network observation is closed");(*env)->DeleteLocalRef(env,error);}}
static void notify(uint64_t token,uint64_t snapshot){
 pthread_mutex_lock(&lock);JavaVM *machine=owner&&epoch==token?vm:NULL;pthread_mutex_unlock(&lock);
 if(!machine)return;
 JNIEnv *env=NULL;int attached=0;jint result=(*machine)->GetEnv(machine,(void**)&env,JNI_VERSION_1_6);
 if(result==JNI_EDETACHED){if((*machine)->AttachCurrentThreadAsDaemon(machine,(void**)&env,NULL)!=JNI_OK)return;attached=1;}else if(result!=JNI_OK)return;
 pthread_mutex_lock(&lock);jclass target=owner?(*env)->NewLocalRef(env,owner):NULL;jmethodID call=changed;pthread_mutex_unlock(&lock);
 if(target&&!(*env)->ExceptionCheck(env))(*env)->CallStaticVoidMethod(env,target,call,token,(jlong)snapshot);
 if((*env)->ExceptionCheck(env)){
  jthrowable failure=(*env)->ExceptionOccurred(env);(*env)->ExceptionClear(env);
  pthread_mutex_lock(&lock);if(!closed&&!pending_failure){pending_failure=(*env)->NewGlobalRef(env,failure);fprintf(stderr,"[android-host] Failure: Passive network notification failed\n");}pthread_mutex_unlock(&lock);
  (*env)->DeleteLocalRef(env,failure);if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
 }
 if(target)(*env)->DeleteLocalRef(env,target);if(attached)(*machine)->DetachCurrentThread(machine);
}
static int observe(JNIEnv *env){int error=network_observe();if(!error)return 1;if(error==ECANCELED)unavailable(env);else{jclass type=(*env)->FindClass(env,"java/lang/OutOfMemoryError");if(type){(*env)->ThrowNew(env,type,"Network observer allocation failed");(*env)->DeleteLocalRef(env,type);}}return 0;}
static int failure(JNIEnv *env){if(pending_failure){(*env)->Throw(env,pending_failure);return 1;}return 0;}
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostConnectivityManager_nativeSnapshot(JNIEnv *env,jclass cls){if(!observe(env))return 0;pthread_mutex_lock(&lock);if(closed){pthread_mutex_unlock(&lock);unavailable(env);return 0;}uint64_t value=failure(env)?0:packed();pthread_mutex_unlock(&lock);return(jlong)value;}
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostConnectivityManager_nativeStart(JNIEnv *env,jclass cls,jlong token){
 JavaVM *machine=NULL;if((*env)->GetJavaVM(env,&machine)!=JNI_OK)return 0;
 jmethodID call=(*env)->GetStaticMethodID(env,cls,"changed","(JJ)V");if(!call)return 0;
 jclass target=(*env)->NewGlobalRef(env,cls);if(!target)return 0;if(!observe(env)){(*env)->DeleteGlobalRef(env,target);return 0;}
 pthread_mutex_lock(&lock);if(closed){pthread_mutex_unlock(&lock);(*env)->DeleteGlobalRef(env,target);unavailable(env);return 0;}
 if(failure(env)){pthread_mutex_unlock(&lock);(*env)->DeleteGlobalRef(env,target);return 0;}
 if(owner){pthread_mutex_unlock(&lock);(*env)->DeleteGlobalRef(env,target);jclass error=(*env)->FindClass(env,"java/lang/IllegalStateException");if(error){(*env)->ThrowNew(env,error,"Network observer is already registered");(*env)->DeleteLocalRef(env,error);}return 0;}
 owner=target;changed=call;epoch=(uint64_t)token;vm=machine;listener=notify;uint64_t value=packed();pthread_mutex_unlock(&lock);return(jlong)value;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostConnectivityManager_nativeStop(JNIEnv *env,jclass cls,jlong token){pthread_mutex_lock(&lock);if(epoch==(uint64_t)token){jclass previous=owner;owner=NULL;changed=NULL;epoch=0;listener=NULL;if(previous)(*env)->DeleteGlobalRef(env,previous);}pthread_mutex_unlock(&lock);}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostConnectivityManager_nativeClose(JNIEnv *env,jclass cls){
 (void)cls;pthread_mutex_lock(&lock);jclass previous=owner;jthrowable failed=pending_failure;owner=NULL;changed=NULL;pending_failure=NULL;pthread_mutex_unlock(&lock);
 network_close();if(previous)(*env)->DeleteGlobalRef(env,previous);if(failed)(*env)->DeleteGlobalRef(env,failed);
}
