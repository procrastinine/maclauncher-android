#include <jni.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
/* No atrace collector is attached. Keep the complete Android Trace class and
 * its validation/dispatch; only the platform event sink is disabled. */
JNIEXPORT jboolean JNICALL Java_android_os_Trace_nativeIsTagEnabled(JNIEnv* e,jclass c,jlong tag){(void)e;(void)c;(void)tag;return JNI_FALSE;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeSetAppTracingAllowed(JNIEnv* e,jclass c,jboolean allowed){(void)e;(void)c;(void)allowed;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeSetTracingEnabled(JNIEnv* e,jclass c,jboolean enabled){(void)e;(void)c;(void)enabled;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeTraceCounter(JNIEnv* e,jclass c,jlong tag,jstring name,jlong value){(void)e;(void)c;(void)tag;(void)name;(void)value;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeTraceBegin(JNIEnv* e,jclass c,jlong tag,jstring name){(void)e;(void)c;(void)tag;(void)name;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeTraceEnd(JNIEnv* e,jclass c,jlong tag){(void)e;(void)c;(void)tag;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeAsyncTraceBegin(JNIEnv* e,jclass c,jlong tag,jstring name,jint cookie){(void)e;(void)c;(void)tag;(void)name;(void)cookie;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeAsyncTraceEnd(JNIEnv* e,jclass c,jlong tag,jstring name,jint cookie){(void)e;(void)c;(void)tag;(void)name;(void)cookie;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeAsyncTraceForTrackBegin(JNIEnv* e,jclass c,jlong tag,jstring track,jstring name,jint cookie){(void)e;(void)c;(void)tag;(void)track;(void)name;(void)cookie;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeAsyncTraceForTrackEnd(JNIEnv* e,jclass c,jlong tag,jstring track,jint cookie){(void)e;(void)c;(void)tag;(void)track;(void)cookie;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeInstant(JNIEnv* e,jclass c,jlong tag,jstring name){(void)e;(void)c;(void)tag;(void)name;}
JNIEXPORT void JNICALL Java_android_os_Trace_nativeInstantForTrack(JNIEnv* e,jclass c,jlong tag,jstring track,jstring name){(void)e;(void)c;(void)tag;(void)track;(void)name;}
/* No Perfetto collector runs in a game process. Categories retain real native
 * allocation/registration lifetime; the upstream disabled builder handles all
 * optional events. This does not advertise trace collection or completed traces. */
typedef struct {atomic_int registered;} category;
static void error(JNIEnv* e,const char* type,const char* text){jclass c=(*e)->FindClass(e,type);if(c){(*e)->ThrowNew(e,c,text);(*e)->DeleteLocalRef(e,c);}}
static void destroy(void* p){free(p);}
JNIEXPORT jlong JNICALL Java_android_os_PerfettoTrace_00024Category_native_1delete(JNIEnv* e,jclass c){(void)e;(void)c;return (jlong)(uintptr_t)destroy;}
JNIEXPORT jlong JNICALL Java_android_os_PerfettoTrace_00024Category_native_1init(JNIEnv* e,jclass c,jstring name,jstring tag,jstring severity){(void)c;(void)tag;(void)severity;if(!name){error(e,"java/lang/NullPointerException","Trace category name is null");return 0;}category* p=calloc(1,sizeof(*p));if(!p)error(e,"java/lang/OutOfMemoryError","Trace category allocation failed");return (jlong)(uintptr_t)p;}
JNIEXPORT jlong JNICALL Java_android_os_PerfettoTrace_00024Category_native_1get_1extra_1ptr(JNIEnv* e,jclass c,jlong p){(void)e;(void)c;return p;}
JNIEXPORT jboolean JNICALL Java_android_os_PerfettoTrace_00024Category_native_1is_1enabled(JNIEnv* e,jclass c,jlong p){(void)e;(void)c;(void)p;return JNI_FALSE;}
JNIEXPORT void JNICALL Java_android_os_PerfettoTrace_00024Category_native_1register(JNIEnv* e,jclass c,jlong p){(void)e;(void)c;if(p)atomic_store(&((category*)(uintptr_t)p)->registered,1);}
JNIEXPORT void JNICALL Java_android_os_PerfettoTrace_00024Category_native_1unregister(JNIEnv* e,jclass c,jlong p){(void)e;(void)c;if(p)atomic_store(&((category*)(uintptr_t)p)->registered,0);}
JNIEXPORT jlong JNICALL Java_android_os_PerfettoTrackEventExtra_native_1delete(JNIEnv* e,jclass c){(void)e;(void)c;return (jlong)(uintptr_t)destroy;}
JNIEXPORT void JNICALL Java_android_os_PerfettoTrace_native_1register(JNIEnv* e,jclass c,jboolean system){(void)e;(void)c;(void)system;}
JNIEXPORT void JNICALL Java_android_os_PerfettoTrace_native_1activate_1trigger(JNIEnv* e,jclass c,jstring trigger,jint timeout){(void)e;(void)c;(void)trigger;(void)timeout;}
JNIEXPORT jlong JNICALL Java_android_os_PerfettoTrace_native_1start_1session(JNIEnv* e,jclass c,jboolean system,jbyteArray config){(void)c;(void)system;(void)config;error(e,"java/lang/UnsupportedOperationException","A Perfetto tracing service is not available");return 0;}
JNIEXPORT jbyteArray JNICALL Java_android_os_PerfettoTrace_native_1stop_1session(JNIEnv* e,jclass c,jlong session){(void)c;(void)session;error(e,"java/lang/IllegalStateException","No Perfetto tracing session is active");return NULL;}

/* The Java Perfetto producer API is also used by ViewRoot and ProtoLog, even
 * without a tracing service. Keep its normal inactive state. Data sources own
 * real native records, retired by the original allocation registry; they do
 * not retain their Java owner or allocate per-frame packets, workers or TLS. */
typedef struct { atomic_int registered; } data_source;
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
static jobject source_registry;
static jmethodID register_allocation;

static int prepare_registry(JNIEnv *env,jclass owner) {
    pthread_mutex_lock(&registry_lock);
    int ready=source_registry!=NULL;
    pthread_mutex_unlock(&registry_lock);
    if(ready)return 1;
    jclass type=(*env)->FindClass(env,"libcore/util/NativeAllocationRegistry");
    if(!type)return 0;
    jmethodID create=(*env)->GetStaticMethodID(env,type,"createMalloced",
        "(Ljava/lang/Class;J)Llibcore/util/NativeAllocationRegistry;");
    jmethodID registration=create?(*env)->GetMethodID(env,type,"registerNativeAllocation",
        "(Ljava/lang/Object;J)Ljava/lang/Runnable;"):NULL;
    if(!registration){(*env)->DeleteLocalRef(env,type);return 0;}
    jobject local=(*env)->CallStaticObjectMethod(env,type,create,owner,(jlong)(uintptr_t)destroy);
    if((*env)->ExceptionCheck(env)){(*env)->DeleteLocalRef(env,type);return 0;}
    jobject global=local?(*env)->NewGlobalRef(env,local):NULL;
    if(local)(*env)->DeleteLocalRef(env,local);
    (*env)->DeleteLocalRef(env,type);
    if(!global)return 0;
    pthread_mutex_lock(&registry_lock);
    if(!source_registry){source_registry=global;register_allocation=registration;global=NULL;}
    pthread_mutex_unlock(&registry_lock);
    if(global)(*env)->DeleteGlobalRef(env,global);
    return 1;
}

JNIEXPORT void JNICALL Java_android_tracing_perfetto_Producer_nativePerfettoProducerInit(
        JNIEnv *env,jclass owner,jint backends,jint size) {
    (void)env;(void)owner;(void)backends;(void)size;
    /* No system or in-process collector is installed. Initialization does not
     * enable any data source, matching a producer with no active sessions. */
}
JNIEXPORT jlong JNICALL Java_android_tracing_perfetto_DataSource_nativeCreate(
        JNIEnv *env,jclass owner,jobject source,jstring name) {
    if(!source||!name){error(env,"java/lang/NullPointerException","Trace data source is null");return 0;}
    if(!prepare_registry(env,owner))return 0;
    data_source *record=calloc(1,sizeof(*record));
    if(!record){error(env,"java/lang/OutOfMemoryError","Trace data source allocation failed");return 0;}
    jobject cleanup=(*env)->CallObjectMethod(env,source_registry,register_allocation,
        source,(jlong)(uintptr_t)record);
    /* registerNativeAllocation retires the pointer itself on allocation failure. */
    if((*env)->ExceptionCheck(env))return 0;
    if(cleanup)(*env)->DeleteLocalRef(env,cleanup);
    return (jlong)(uintptr_t)record;
}
JNIEXPORT jlong JNICALL Java_android_tracing_perfetto_DataSource_nativeGetFinalizer(
        JNIEnv *env,jclass owner){(void)env;(void)owner;return (jlong)(uintptr_t)destroy;}
JNIEXPORT void JNICALL Java_android_tracing_perfetto_DataSource_nativeRegisterDataSource(
        JNIEnv *env,jclass owner,jlong value,jint policy,jboolean notify,jboolean no_flush) {
    (void)env;(void)owner;(void)policy;(void)notify;(void)no_flush;
    if(value)atomic_store(&((data_source*)(uintptr_t)value)->registered,1);
}
JNIEXPORT void JNICALL Java_android_tracing_perfetto_DataSource_nativeFlushAll(
        JNIEnv *env,jclass owner,jlong value){(void)env;(void)owner;(void)value;}
JNIEXPORT jobject JNICALL Java_android_tracing_perfetto_DataSource_nativeGetPerfettoInstanceLocked(
        JNIEnv *env,jclass owner,jlong value,jint index){(void)env;(void)owner;(void)value;(void)index;return NULL;}
JNIEXPORT void JNICALL Java_android_tracing_perfetto_DataSource_nativeReleasePerfettoInstanceLocked(
        JNIEnv *env,jclass owner,jlong value,jint index){(void)env;(void)owner;(void)value;(void)index;}
JNIEXPORT jboolean JNICALL Java_android_tracing_perfetto_DataSource_nativePerfettoDsTraceIterateBegin(
        JNIEnv *env,jclass owner,jlong value){(void)env;(void)owner;(void)value;return JNI_FALSE;}
JNIEXPORT jboolean JNICALL Java_android_tracing_perfetto_DataSource_nativePerfettoDsTraceIterateNext(
        JNIEnv *env,jclass owner,jlong value){(void)env;(void)owner;(void)value;return JNI_FALSE;}
JNIEXPORT void JNICALL Java_android_tracing_perfetto_DataSource_nativePerfettoDsTraceIterateBreak(
        JNIEnv *env,jclass owner,jlong value){(void)env;(void)owner;(void)value;}
JNIEXPORT jint JNICALL Java_android_tracing_perfetto_DataSource_nativeGetPerfettoDsInstanceIndex(
        JNIEnv *env,jclass owner,jlong value){(void)env;(void)owner;(void)value;return -1;}
JNIEXPORT void JNICALL Java_android_tracing_perfetto_DataSource_nativeWritePackets(
        JNIEnv *env,jclass owner,jlong value,jobjectArray packets){(void)env;(void)owner;(void)value;(void)packets;}
/* Contexts exist only inside an active trace callback. There are no such
 * callbacks without a collector, so no per-thread state exists to retain. */
JNIEXPORT jobject JNICALL Java_android_tracing_perfetto_TracingContext_nativeGetCustomTls(
        JNIEnv *env,jclass owner,jlong value){(void)env;(void)owner;(void)value;return NULL;}
JNIEXPORT jobject JNICALL Java_android_tracing_perfetto_TracingContext_nativeGetIncrementalState(
        JNIEnv *env,jclass owner,jlong value){(void)env;(void)owner;(void)value;return NULL;}
JNIEXPORT void JNICALL Java_android_tracing_perfetto_TracingContext_nativeSetCustomTls(
        JNIEnv *env,jclass owner,jlong value,jobject state){(void)env;(void)owner;(void)value;(void)state;}
JNIEXPORT void JNICALL Java_android_tracing_perfetto_TracingContext_nativeSetIncrementalState(
        JNIEnv *env,jclass owner,jlong value,jobject state){(void)env;(void)owner;(void)value;(void)state;}
