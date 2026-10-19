#include "signal-bridge.h"
#include <jni.h>
#include <unistd.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <fcntl.h>
#include "native-clock/clock-domain.h"
#include "process-identity.h"
#include "process-ids.h"
#include "thread-identity.h"
/* Original Process/SystemClock algorithms share Android process identities,
 * native scheduling and the same Mach clock domains as Klepton and Java nanoTime. */
static void error(JNIEnv* e,const char* type,const char* message){if((*e)->ExceptionCheck(e))return;jclass c=(*e)->FindClass(e,type);if(c){(*e)->ThrowNew(e,c,message);(*e)->DeleteLocalRef(e,c);}}
static jint tid(JNIEnv* e){int value=klj_thread_id();if(value<0){error(e,"java/lang/IllegalStateException","Native thread identity allocation failed");return 0;}return value;}
#define ID(name,operation) JNIEXPORT jint JNICALL Java_libcore_io_Linux_##name(JNIEnv* e,jclass c){(void)e;(void)c;return (jint)(operation);}
ID(nativeGetpid,klj_process_id()) ID(nativeGetppid,klj_process_parent_id()) ID(nativeGetuid,klj_process_uid()) ID(nativeGeteuid,klj_process_uid()) ID(nativeGetgid,klj_process_gid()) ID(nativeGetegid,klj_process_gid()) ID(nativeGettid,tid(e))
JNIEXPORT jboolean JNICALL Java_dalvik_system_VMRuntime_is64Bit(JNIEnv* e,jobject c){(void)e;(void)c;return sizeof(void*)==8;}
/* Hidden-API warning controls belong to ART's access checker. HotSpot does not
 * run that checker: all supplied framework APIs retain ordinary JVM access.
 * Keep original StrictMode policy transitions executable without introducing a
 * second access policy or claiming that ART warning sampling is available. */
static void hidden_api_warning_controls(void){
    static atomic_flag reported=ATOMIC_FLAG_INIT;
    if(!atomic_flag_test_and_set_explicit(&reported,memory_order_relaxed))
        fprintf(stderr,"[android-host] ART hidden-API warning controls are unavailable; framework access uses JVM visibility.\n");
}
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_setDedupeHiddenApiWarnings(JNIEnv* e,jclass c,jboolean enabled){(void)e;(void)c;(void)enabled;hidden_api_warning_controls();}
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_setHiddenApiAccessLogSamplingRate(JNIEnv* e,jobject c,jint rate){(void)e;(void)c;(void)rate;hidden_api_warning_controls();}
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_setHiddenApiExemptions(JNIEnv* e,jobject c,jobjectArray prefixes){(void)e;(void)c;(void)prefixes;hidden_api_warning_controls();}
/* ART notifications describe lifecycle/profile events to its GC and compiler.
 * HotSpot owns those optimizers and does not consume ART profiles or DEX caches.
 * Keep the original framework's lifecycle transitions intact. A notification
 * must not force a full collection, reset the JVM compiler, or become fatal. */
static void art_optimizer_notification(void){
    static atomic_flag reported=ATOMIC_FLAG_INIT;
    if(!atomic_flag_test_and_set_explicit(&reported,memory_order_relaxed))
        fprintf(stderr,"[android-host] ART optimization hints are unavailable; the JVM manages compilation and garbage collection.\n");
}
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_updateProcessState(JNIEnv* e,jobject c,jint state){(void)e;(void)c;(void)state;art_optimizer_notification();}
#define ART_NOTIFICATION(name) JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_##name(JNIEnv* e,jobject c){(void)e;(void)c;art_optimizer_notification();}
ART_NOTIFICATION(notifyStartupCompleted)
ART_NOTIFICATION(bootCompleted)
ART_NOTIFICATION(preloadDexCaches)
ART_NOTIFICATION(resetJitCounters)
ART_NOTIFICATION(registerSensitiveThread)
JNIEXPORT void JNICALL Java_dalvik_system_VMRuntime_registerAppInfo(JNIEnv* e,jclass c,jstring package,jstring profile,jstring reference,jobjectArray paths,jint codeType){(void)e;(void)c;(void)package;(void)profile;(void)reference;(void)paths;(void)codeType;art_optimizer_notification();}
#undef ART_NOTIFICATION
static jlong clock_value(JNIEnv* e,int clock,uint64_t divisor){struct timespec v;if(klj_clock_read(clock,&v)){error(e,"java/lang/IllegalStateException","Host clock query failed");return 0;}return (jlong)(((uint64_t)v.tv_sec*1000000000ull+v.tv_nsec)/divisor);}
#define CLOCK(name,domain,divisor) JNIEXPORT jlong JNICALL Java_android_os_SystemClock_##name(JNIEnv* e,jclass c){(void)c;return clock_value(e,domain,divisor);}
CLOCK(uptimeMillis,1,1000000) CLOCK(uptimeNanos,1,1) CLOCK(elapsedRealtime,7,1000000) CLOCK(elapsedRealtimeNanos,7,1) CLOCK(currentThreadTimeMillis,3,1000000) CLOCK(currentThreadTimeMicro,3,1000) CLOCK(currentTimeMicro,0,1000)
/* The game host has no Android/DDMS debugger transport or ART method tracer.
 * These are absence queries used by framework and application diagnostics; they
 * never create a listener, suspend a game, or query management beans per call. */
JNIEXPORT jboolean JNICALL Java_dalvik_system_VMDebug_isDebuggerConnected(JNIEnv* e,jclass c){(void)e;(void)c;return JNI_FALSE;}
JNIEXPORT jboolean JNICALL Java_dalvik_system_VMDebug_isDebuggingEnabled(JNIEnv* e,jclass c){(void)e;(void)c;return JNI_FALSE;}
JNIEXPORT jlong JNICALL Java_dalvik_system_VMDebug_lastDebuggerActivity(JNIEnv* e,jclass c){(void)e;(void)c;return -1;}
JNIEXPORT jlong JNICALL Java_dalvik_system_VMDebug_threadCpuTimeNanos(JNIEnv* e,jclass c){(void)c;return clock_value(e,3,1);}
JNIEXPORT jint JNICALL Java_dalvik_system_VMDebug_getMethodTracingMode(JNIEnv* e,jclass c){(void)e;(void)c;return 0;}
JNIEXPORT jobjectArray JNICALL Java_dalvik_system_VMDebug_getVmFeatureList(JNIEnv* e,jclass c){(void)c;jclass strings=(*e)->FindClass(e,"java/lang/String");if(!strings)return NULL;jobjectArray result=(*e)->NewObjectArray(e,0,strings,NULL);(*e)->DeleteLocalRef(e,strings);return result;}
JNIEXPORT jlong JNICALL Java_android_os_Process_getElapsedCpuTime(JNIEnv* e,jclass c){(void)c;return clock_value(e,2,1000000);}
/* Original android_util_Process.cpp builds with GUARD_THREAD_PRIORITY=0.
 * ServiceThread still calls this diagnostic hook before HandlerThread applies
 * its requested priority, including legitimate background service workers. */
JNIEXPORT void JNICALL Java_android_os_Process_setCanSelfBackground(JNIEnv* e,jclass c,jboolean allowed){(void)e;(void)c;(void)allowed;}
static void priority_error(JNIEnv* e,int code){
    if(code)error(e,code==EINVAL||code==ESRCH?"java/lang/IllegalArgumentException":"java/lang/SecurityException","Host thread scheduling request failed");
}
static void priority(JNIEnv* e,int target,int value){
    if(value<-20||value>19){error(e,"java/lang/IllegalArgumentException","Thread priority is outside Android range");return;}
    priority_error(e,klj_thread_priority_set(target,value));
}
JNIEXPORT void JNICALL Java_android_os_Process_setThreadPriority__I(JNIEnv* e,jclass c,jint value){(void)c;priority(e,0,value);}
JNIEXPORT void JNICALL Java_android_os_Process_setThreadPriority__II(JNIEnv* e,jclass c,jint target,jint value){(void)c;priority(e,target,value);}
JNIEXPORT jint JNICALL Java_android_os_Process_getThreadPriority(JNIEnv* e,jclass c,jint target){
    (void)c;int value=0;priority_error(e,klj_thread_priority_get(target,&value));return value;
}
JNIEXPORT jint JNICALL Java_android_os_Process_getThreadScheduler(JNIEnv* e,jclass c,jint target){
    (void)c;int policy=0,value=0;priority_error(e,klj_thread_scheduler_get(target,&policy,&value,NULL));return policy;
}
JNIEXPORT void JNICALL Java_android_os_Process_setThreadScheduler(JNIEnv* e,jclass c,jint target,jint policy,jint value){
    (void)c;priority_error(e,klj_thread_scheduler_set(target,policy,value));
}
/* Android task profiles become actual Darwin background policy. They neither
 * claim a Linux cpuset nor grant realtime scheduling: the original service
 * separately applies setThreadScheduler and handles genuine kernel refusal. */
static void thread_group(JNIEnv* e,int target,int group){
    if(group < -1 || group > 8){priority_error(e,EINVAL);return;}
    thread_t thread=klj_thread_acquire(target);
    if(!MACH_PORT_VALID(thread)){priority_error(e,errno?errno:ESRCH);return;}
    thread_background_policy_data_t policy={group==0||group==7?THREAD_BACKGROUND_POLICY_DARWIN_BG:0};
    kern_return_t result=thread_policy_set(thread,THREAD_BACKGROUND_POLICY,(thread_policy_t)&policy,THREAD_BACKGROUND_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(),thread);
    priority_error(e,result==KERN_SUCCESS?0:result==KERN_INVALID_ARGUMENT?EINVAL:EPERM);
}
JNIEXPORT void JNICALL Java_android_os_Process_setThreadGroup(JNIEnv* e,jclass c,jint target,jint group){(void)c;thread_group(e,target,group);}
JNIEXPORT void JNICALL Java_android_os_Process_setThreadGroupAndCpuset(JNIEnv* e,jclass c,jint target,jint group){(void)c;thread_group(e,target,group);}
/* Keep the original readProcLines contract: absent files leave caller
 * sentinels intact; a readable file zeros absent fields and reads the first
 * decimal value of each requested line. Only /proc identity data is adapted;
 * the existing authenticated process lease owns every returned PID and UID. */
JNIEXPORT void JNICALL Java_android_os_Process_readProcLines(JNIEnv* e,jclass c,jstring file,jobjectArray fields,jlongArray output){
    (void)c;if(!file||!fields||!output){error(e,"java/lang/NullPointerException",NULL);return;}
    jsize count=(*e)->GetArrayLength(e,fields);
    if(count>(*e)->GetArrayLength(e,output)){error(e,"java/lang/IllegalArgumentException","Array lengths differ");return;}
    for(jsize i=0;i<count;i++){jobject field=(*e)->GetObjectArrayElement(e,fields,i);if(!field){error(e,"java/lang/NullPointerException","Element in reqFields");return;}(*e)->DeleteLocalRef(e,field);}
    const char *name=(*e)->GetStringUTFChars(e,file,NULL);if(!name)return;
    char buffer[4096];ssize_t length=-1;
    if(!strncmp(name,"/proc/",6)){
        const char *at=name+6;char *end;long id;
        if(!strncmp(at,"self/",5)){id=klj_process_id();end=(char*)at+4;}
        else if(!strncmp(at,"thread-self/",12)){id=klj_thread_id();end=(char*)at+11;}
        else id=strtol(at,&end,10);
        if(id>0&&id<=INT_MAX&&end!=at&&!strcmp(end,"/status")){
            pid_t physical;int kind,leader;uid_t uid;
            if(!klj_process_signal_owner((int)id,&physical,&kind,&leader,&uid))
                length=snprintf(buffer,sizeof buffer,"Tgid:\t%d\nPid:\t%ld\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\n",leader,id,uid,uid,uid,uid,uid,uid,uid,uid);
        }
    } else {
        jclass type=(*e)->FindClass(e,"maclauncher/android/framework/HostFilesystem");
        jmethodID map=type?(*e)->GetStaticMethodID(e,type,"pathBytes","(Ljava/lang/String;)[B"):NULL;
        jbyteArray bytes=map?(*e)->CallStaticObjectMethod(e,type,map,file):NULL;
        if(bytes&&!(*e)->ExceptionCheck(e)){
            jsize size=(*e)->GetArrayLength(e,bytes);char *mapped=malloc((size_t)size+1);
            if(mapped){(*e)->GetByteArrayRegion(e,bytes,0,size,(jbyte*)mapped);mapped[size]=0;
                int fd=open(mapped,O_RDONLY|O_CLOEXEC);free(mapped);if(fd>=0){length=read(fd,buffer,sizeof buffer-1);close(fd);if(length<0)length=0;}}
            else error(e,"java/lang/OutOfMemoryError",NULL);
        }
        if(bytes)(*e)->DeleteLocalRef(e,bytes);if(type)(*e)->DeleteLocalRef(e,type);
    }
    (*e)->ReleaseStringUTFChars(e,file,name);
    if(length<0||(*e)->ExceptionCheck(e))return;buffer[length]=0;
    jlong *values=(*e)->GetLongArrayElements(e,output,NULL);if(!values)return;
    for(jsize i=0;i<count;i++){
        values[i]=0;jstring field=(*e)->GetObjectArrayElement(e,fields,i);
        const char *key=(*e)->GetStringUTFChars(e,field,NULL);
        if(!key){(*e)->DeleteLocalRef(e,field);break;}
        size_t size=strlen(key);
        for(const char *line=buffer;*line;){if(!strncmp(line,key,size)){values[i]=strtoll(line+size,NULL,10);break;}const char *next=strchr(line,'\n');if(!next)break;line=next+1;}
        (*e)->ReleaseStringUTFChars(e,field,key);(*e)->DeleteLocalRef(e,field);
    }
    (*e)->ReleaseLongArrayElements(e,output,values,0);
}
JNIEXPORT jintArray JNICALL Java_android_os_Process_getPidsForCommands(JNIEnv* e,jclass c,jobjectArray input){
    (void)c;if(!input){error(e,"java/lang/NullPointerException",NULL);return NULL;}
    jsize count=(*e)->GetArrayLength(e,input);const char **names=calloc((size_t)count+1,sizeof *names);
    if(!names){error(e,"java/lang/OutOfMemoryError",NULL);return NULL;}
    jintArray result=NULL;int *pids=NULL;size_t length=0;
    for(jsize i=0;i<count;i++){
        jstring name=(*e)->GetObjectArrayElement(e,input,i);
        if(!name){error(e,"java/lang/NullPointerException","Element in commandNames");goto done;}
        const char *text=(*e)->GetStringUTFChars(e,name,NULL);names[i]=text?strdup(text):NULL;
        if(text)(*e)->ReleaseStringUTFChars(e,name,text);(*e)->DeleteLocalRef(e,name);
        if(!names[i]){if(!(*e)->ExceptionCheck(e))error(e,"java/lang/OutOfMemoryError",NULL);goto done;}
    }
    if(klj_process_find_commands(names,(size_t)count,&pids,&length)){fprintf(stderr,"[android-host] Android process commands: %s\n",strerror(errno));goto done;}
    result=(*e)->NewIntArray(e,(jsize)length);
    if(result&&length)(*e)->SetIntArrayRegion(e,result,0,(jsize)length,(jint*)pids);
done:
    for(jsize i=0;i<count;i++)free((void*)names[i]);free(names);free(pids);return result;
}
static void send_signal(JNIEnv* e,int pid,int sig,int throwing){
    int host=klj_signal_to_host(sig);
    if(pid<=0||host<0){if(throwing)error(e,"java/lang/IllegalArgumentException","Invalid process or signal");return;}
    /* Native imports and Java Process use the same retained Android child
     * leases. The owner validates the target before resolving its host PID. */
    if(klj_process_signal(pid,host)&&throwing)
        error(e,errno==ESRCH?"java/util/NoSuchElementException":errno==EINVAL?"java/lang/IllegalArgumentException":"java/lang/SecurityException","Android process signal failed");
}
JNIEXPORT void JNICALL Java_android_os_Process_sendSignal(JNIEnv* e,jclass c,jint p,jint s){(void)c;send_signal(e,p,s,0);}
JNIEXPORT void JNICALL Java_android_os_Process_sendSignalQuiet(JNIEnv* e,jclass c,jint p,jint s){(void)c;send_signal(e,p,s,0);}
JNIEXPORT void JNICALL Java_android_os_Process_sendSignalThrows(JNIEnv* e,jclass c,jint p,jint s){(void)c;send_signal(e,p,s,1);}
JNIEXPORT void JNICALL Java_android_os_Process_sendTgSignalThrows(JNIEnv* e,jclass c,jint p,jint t,jint s){
    (void)c;
    if(klj_signal_thread_id(p,t,s))error(e,errno==ESRCH||errno==EINVAL?"java/lang/IllegalArgumentException":"java/lang/SecurityException","Android thread signal failed");
}
static jint id_name(JNIEnv* e,jstring value,int group){
    if(!value){error(e,"java/lang/NullPointerException",NULL);return -1;}
    const char* name=(*e)->GetStringUTFChars(e,value,NULL);if(!name)return -1;
    /* android_util_Process.cpp treats only ASCII digits as a numeric ID;
     * every other spelling uses the original Bionic account database. */
    int result=-1;size_t length=strlen(name);
    if(length){
        size_t i=0;while(i<length&&name[i]>='0'&&name[i]<='9')i++;
        result=i==length?atoi(name):klj_database_name_id(name,group);
    }
    (*e)->ReleaseStringUTFChars(e,value,name);return result;
}
JNIEXPORT jint JNICALL Java_android_os_Process_getUidForName(JNIEnv* e,jclass c,jstring name){(void)c;return id_name(e,name,0);}
JNIEXPORT jint JNICALL Java_android_os_Process_getGidForName(JNIEnv* e,jclass c,jstring name){(void)c;return id_name(e,name,1);}
static void process_name(JNIEnv* e,jstring name){if(!name){error(e,"java/lang/NullPointerException","Process name is null");return;}const char* value=(*e)->GetStringUTFChars(e,name,NULL);if(!value)return;if(klj_process_name(value)){(*e)->ReleaseStringUTFChars(e,name,value);error(e,"java/lang/OutOfMemoryError","Android process name allocation failed");return;}char bounded[64];size_t n=strlen(value);if(n>63)n=63;memcpy(bounded,value,n);bounded[n]=0;pthread_setname_np(bounded);(*e)->ReleaseStringUTFChars(e,name,value);}
JNIEXPORT void JNICALL Java_android_os_Process_setArgV0Native(JNIEnv* e,jclass c,jstring name){(void)c;process_name(e,name);}
JNIEXPORT void JNICALL Java_dalvik_system_VMDebug_setCurrentProcessName(JNIEnv* e,jclass c,jstring name){(void)c;process_name(e,name);}
JNIEXPORT jint JNICALL Java_android_os_Process_setUid(JNIEnv* e,jclass c,jint uid){(void)e;(void)c;return klj_process_setuid(uid);}
JNIEXPORT jint JNICALL Java_android_os_Process_setGid(JNIEnv* e,jclass c,jint gid){(void)e;(void)c;return klj_process_setgid(gid);}
JNIEXPORT jlong JNICALL Java_android_os_Process_getTotalMemory(JNIEnv* e,jclass c){(void)c;uint64_t value=0;size_t size=sizeof(value);if(sysctlbyname("hw.memsize",&value,&size,NULL,0)){error(e,"java/lang/IllegalStateException","Physical memory query failed");return -1;}return value;}
JNIEXPORT jlong JNICALL Java_android_os_Process_getFreeMemory(JNIEnv* e,jclass c){(void)c;vm_statistics64_data_t v;mach_msg_type_number_t n=HOST_VM_INFO64_COUNT;mach_port_t h=mach_host_self();kern_return_t code=host_statistics64(h,HOST_VM_INFO64,(host_info64_t)&v,&n);mach_port_deallocate(mach_task_self(),h);if(code!=KERN_SUCCESS){error(e,"java/lang/IllegalStateException","Available memory query failed");return -1;}return ((uint64_t)v.free_count+v.inactive_count+v.speculative_count)*(uint64_t)getpagesize();}
JNIEXPORT jint JNICALL Java_android_os_Process_nativePidFdOpen(JNIEnv* e,jclass c,jint p,jint flags){(void)c;(void)p;(void)flags;jclass type=(*e)->FindClass(e,"android/system/ErrnoException");jmethodID init=type?(*e)->GetMethodID(e,type,"<init>","(Ljava/lang/String;I)V"):NULL;jstring op=init?(*e)->NewStringUTF(e,"pidfd_open"):NULL;if(op){jthrowable ex=(*e)->NewObject(e,type,init,op,38);if(ex){(*e)->Throw(e,ex);(*e)->DeleteLocalRef(e,ex);}(*e)->DeleteLocalRef(e,op);}if(type)(*e)->DeleteLocalRef(e,type);return -1;}
