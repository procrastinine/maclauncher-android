#include <jni.h>
#include <stdatomic.h>
#include <unistd.h>

/* One JVM owns this process-lifetime class reference. Initialization is serialized
 * by JvmNativeMemory; reserve/unreserve themselves use Java 21 Bits' atomic budget,
 * reference processing and MaxDirectMemorySize limit. No second byte counter. */
static _Atomic(jclass) bits;
static jmethodID reserve_memory, unreserve_memory;

/* HotSpot owns primitive and reference arrays. Consumers which require an ART
 * heap address adapt their native boundary to a scoped JNI array lease. */
JNIEXPORT jobject JNICALL Java_dalvik_system_VMRuntime_newNonMovableArray(JNIEnv *env,jobject owner,jclass component,jint length) {
    (void)owner;
    jclass arrays=(*env)->FindClass(env,"java/lang/reflect/Array");
    if(!arrays)return NULL;
    jmethodID create=(*env)->GetStaticMethodID(env,arrays,"newInstance","(Ljava/lang/Class;I)Ljava/lang/Object;");
    jobject result=create?(*env)->CallStaticObjectMethod(env,arrays,create,component,length):NULL;
    (*env)->DeleteLocalRef(env,arrays);
    return result;
}

JNIEXPORT void JNICALL Java_maclauncher_android_framework_JvmNativeMemory_initializeBudget(JNIEnv *env,jclass owner) {
    (void)owner;
    if(atomic_load_explicit(&bits,memory_order_acquire))return;
    jclass local=(*env)->FindClass(env,"java/nio/Bits");
    if(!local)return;
    jmethodID reserve=(*env)->GetStaticMethodID(env,local,"reserveMemory","(JJ)V");
    if(!reserve){(*env)->DeleteLocalRef(env,local);return;}
    jmethodID unreserve=(*env)->GetStaticMethodID(env,local,"unreserveMemory","(JJ)V");
    if(!unreserve){(*env)->DeleteLocalRef(env,local);return;}
    jclass retained=(*env)->NewGlobalRef(env,local);
    (*env)->DeleteLocalRef(env,local);
    if(!retained)return;
    reserve_memory=reserve;unreserve_memory=unreserve;
    atomic_store_explicit(&bits,retained,memory_order_release);
}

static int ready(JNIEnv *env) {
    if(atomic_load_explicit(&bits,memory_order_acquire))return 1;
    jclass error=(*env)->FindClass(env,"java/lang/IllegalStateException");
    if(error){(*env)->ThrowNew(env,error,"Android native memory budget is not initialized");(*env)->DeleteLocalRef(env,error);}
    return 0;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_JvmNativeMemory_reserve(JNIEnv *env,jclass owner,jlong bytes) {
    (void)owner;
    if(ready(env))(*env)->CallStaticVoidMethod(env,atomic_load_explicit(&bits,memory_order_acquire),reserve_memory,bytes,bytes);
    /* Any OOME remains pending: AOSP frees the just-supplied native allocation. */
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_JvmNativeMemory_unreserve(JNIEnv *env,jclass owner,jlong bytes) {
    (void)owner;
    if(ready(env))(*env)->CallStaticVoidMethod(env,atomic_load_explicit(&bits,memory_order_acquire),unreserve_memory,bytes,bytes);
}

/* Some complete AOSP clients account native memory directly through VMRuntime
 * rather than NativeAllocationRegistry. Keep those original methods and use
 * the same JDK budget; these byte counts carry no IS_MALLOCED tag. */
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerNativeAllocation(JNIEnv *env,jobject owner,jlong bytes) {
    (void)owner;
    Java_maclauncher_android_framework_JvmNativeMemory_initializeBudget(env,NULL);
    if(!(*env)->ExceptionCheck(env))Java_maclauncher_android_framework_JvmNativeMemory_reserve(env,NULL,bytes);
}
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerNativeFree(JNIEnv *env,jobject owner,jlong bytes) {
    (void)owner;
    Java_maclauncher_android_framework_JvmNativeMemory_initializeBudget(env,NULL);
    if(!(*env)->ExceptionCheck(env))Java_maclauncher_android_framework_JvmNativeMemory_unreserve(env,NULL,bytes);
}

/* Preserve BinderInternal's original pressure/GC thresholds and finalizers.
 * HotSpot, not ART, owns this heap. Do not collect synchronously while Binder
 * holds native locks, and do not turn an optional pressure request into an
 * unresolved native method that aborts the service process. */
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_requestConcurrentGC(JNIEnv *env,jobject owner) {
    (void)owner;
    jclass memory=(*env)->FindClass(env,"maclauncher/android/framework/JvmNativeMemory");
    if(!memory)return;
    jmethodID request=(*env)->GetStaticMethodID(env,memory,"requestCollection","()V");
    if(request)(*env)->CallStaticVoidMethod(env,memory,request);
    (*env)->DeleteLocalRef(env,memory);
}

