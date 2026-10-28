#include <jni.h>
#include <malloc/malloc.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#include "process-ids.h"
static malloc_statistics_t snapshot(void){malloc_statistics_t value={0};malloc_zone_statistics(NULL,&value);return value;}
static jlong measured(size_t value){return value>(size_t)INT64_MAX?INT64_MAX:(jlong)value;}
JNIEXPORT jlong JNICALL Java_android_os_Debug_getNativeHeapAllocatedSize(JNIEnv*env,jclass type){(void)env;(void)type;return measured(snapshot().size_in_use);}
JNIEXPORT jlong JNICALL Java_android_os_Debug_getNativeHeapSize(JNIEnv*env,jclass type){(void)env;(void)type;return measured(snapshot().size_allocated);}
JNIEXPORT jlong JNICALL Java_android_os_Debug_getNativeHeapFreeSize(JNIEnv*env,jclass type){(void)env;(void)type;malloc_statistics_t s=snapshot();return measured(s.size_allocated>s.size_in_use?s.size_allocated-s.size_in_use:0);}

/* Debug.MemoryInfo is a view of the owning process VM map, not an allocator
 * ledger. Mach tags identify native/Java regions only when the kernel reports
 * them. No JVM heap allocation is added to the resident pages a second time.
 * Darwin does not expose Linux smaps PSS or proportional swap accounting:
 * those fields remain unavailable (zero), including memtrack-only counters. */
enum { MEMORY_OTHER, MEMORY_JAVA, MEMORY_NATIVE, MEMORY_GROUPS };
enum { PSS, SWAPPABLE_PSS, RSS, PRIVATE_DIRTY, SHARED_DIRTY, PRIVATE_CLEAN,
       SHARED_CLEAN, SWAPPED_OUT, SWAPPED_OUT_PSS, MEMORY_CATEGORIES };
enum { OTHER_STACK=1, OTHER_SO=6, OTHER_UNKNOWN_MAP=13, OTHER_STATS=32 };
typedef struct {
    uint64_t groups[MEMORY_GROUPS][MEMORY_CATEGORIES];
    uint64_t other[OTHER_STATS][MEMORY_CATEGORIES];
    uint64_t resident,unique;
} process_memory;

/* Android PIDs are leases in the supervisor's process namespace. Only this
 * process's own lease names the Mach task measured here. */
static int current_process(pid_t pid){return pid>0&&pid==klj_process_id();}
static int resident_memory(uint64_t *bytes){
    mach_task_basic_info_data_t info={0};mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
    if(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,(task_info_t)&info,&count)!=KERN_SUCCESS)return 0;
    *bytes=info.resident_size;return 1;
}
static unsigned memory_group(unsigned tag){
    switch(tag){
        case VM_MEMORY_MALLOC:case VM_MEMORY_MALLOC_SMALL:case VM_MEMORY_MALLOC_LARGE:
        case VM_MEMORY_MALLOC_HUGE:case VM_MEMORY_MALLOC_TINY:case VM_MEMORY_MALLOC_LARGE_REUSABLE:
        case VM_MEMORY_MALLOC_LARGE_REUSED:case VM_MEMORY_MALLOC_NANO:case VM_MEMORY_MALLOC_MEDIUM:
        case VM_MEMORY_MALLOC_PROB_GUARD:return MEMORY_NATIVE;
        case VM_MEMORY_JAVA:return MEMORY_JAVA;
        default:return MEMORY_OTHER;
    }
}
static int process_snapshot(process_memory *result){
    memset(result,0,sizeof(*result));
    if(!resident_memory(&result->resident))return 0;
    mach_vm_address_t address=0;natural_t depth=0;
    for(;;){
        mach_vm_size_t extent=0;vm_region_submap_info_data_64_t info={0};
        mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t status=mach_vm_region_recurse(mach_task_self(),&address,&extent,&depth,(vm_region_recurse_info_t)&info,&count);
        if(status==KERN_INVALID_ADDRESS)break;
        if(status!=KERN_SUCCESS)return 0;
        if(info.is_submap){depth++;continue;}
        uint64_t values[MEMORY_CATEGORIES]={0};
        uint64_t resident=(uint64_t)info.pages_resident*vm_page_size;
        uint64_t dirty=(uint64_t)info.pages_dirtied*vm_page_size;
        if(dirty>resident)dirty=resident;
        values[RSS]=resident;
        if(info.share_mode==SM_PRIVATE||info.share_mode==SM_PRIVATE_ALIASED){
            result->unique+=resident;values[PRIVATE_DIRTY]=dirty;values[PRIVATE_CLEAN]=resident-dirty;
        }else{
            uint64_t private=(uint64_t)info.pages_shared_now_private*vm_page_size;
            if(private>resident)private=resident;
            result->unique+=private;
            /* For mixed COW objects Mach does not separate dirty pages by
             * sharing status. Keep that unavailable split empty, rather than
             * attributing every dirty page to either owner. */
            if(!private){values[SHARED_DIRTY]=dirty;values[SHARED_CLEAN]=resident-dirty;}
            else if(private==resident){values[PRIVATE_DIRTY]=dirty;values[PRIVATE_CLEAN]=resident-dirty;}
        }
        unsigned group=memory_group(info.user_tag);
        unsigned other=info.user_tag==VM_MEMORY_STACK?OTHER_STACK:info.user_tag==VM_MEMORY_DYLIB?OTHER_SO:OTHER_UNKNOWN_MAP;
        for(unsigned i=0;i<MEMORY_CATEGORIES;i++){
            result->groups[group][i]+=values[i];
            if(group==MEMORY_OTHER)result->other[other][i]+=values[i];
        }
        if(!extent||address>UINT64_MAX-extent)break;
        address+=extent;
    }
    return 1;
}
static jint kilobytes_int(uint64_t value){value/=1024;return value>INT_MAX?INT_MAX:(jint)value;}
static void output_longs(JNIEnv *env,jlongArray target,const jlong *values,jsize count){
    if(!target)return;jsize length=(*env)->GetArrayLength(env,target);if(length<count)count=length;
    if(count)(*env)->SetLongArrayRegion(env,target,0,count,values);
}
static jboolean memory_info(JNIEnv *env,int current,jobject target){
    if(!target){jclass error=(*env)->FindClass(env,"java/lang/NullPointerException");if(error)(*env)->ThrowNew(env,error,"memoryInfo");return JNI_FALSE;}
    process_memory info;if(!current||!process_snapshot(&info))return JNI_FALSE;
    jclass type=(*env)->GetObjectClass(env,target);
    const char *prefixes[]={"other","dalvik","native"};
    const char *suffixes[]={"Pss","SwappablePss","Rss","PrivateDirty","SharedDirty","PrivateClean","SharedClean","SwappedOut","SwappedOutPss"};
    for(unsigned group=0;group<MEMORY_GROUPS;group++)for(unsigned i=0;i<MEMORY_CATEGORIES;i++){
        char name[64];snprintf(name,sizeof(name),"%s%s",prefixes[group],suffixes[i]);
        jfieldID field=(*env)->GetFieldID(env,type,name,"I");if(!field)goto failed;
        (*env)->SetIntField(env,target,field,kilobytes_int(info.groups[group][i]));
    }
    jfieldID proportional=(*env)->GetFieldID(env,type,"hasSwappedOutPss","Z");if(!proportional)goto failed;
    (*env)->SetBooleanField(env,target,proportional,JNI_FALSE);
    jfieldID stats=(*env)->GetFieldID(env,type,"otherStats","[I");if(!stats)goto failed;
    jintArray array=(*env)->GetObjectField(env,target,stats);
    if(array){
        jsize length=(*env)->GetArrayLength(env,array);jint values[OTHER_STATS*MEMORY_CATEGORIES];
        for(unsigned i=0;i<OTHER_STATS*MEMORY_CATEGORIES;i++)values[i]=kilobytes_int(info.other[i/MEMORY_CATEGORIES][i%MEMORY_CATEGORIES]);
        if(length>OTHER_STATS*MEMORY_CATEGORIES)length=OTHER_STATS*MEMORY_CATEGORIES;
        if(length)(*env)->SetIntArrayRegion(env,array,0,length,values);
        (*env)->DeleteLocalRef(env,array);
    }
    (*env)->DeleteLocalRef(env,type);return (*env)->ExceptionCheck(env)?JNI_FALSE:JNI_TRUE;
failed:
    (*env)->DeleteLocalRef(env,type);return JNI_FALSE;
}
JNIEXPORT void JNICALL Java_android_os_Debug_getMemoryInfo__Landroid_os_Debug_00024MemoryInfo_2(JNIEnv *env,jclass type,jobject target){(void)type;memory_info(env,1,target);}
JNIEXPORT jboolean JNICALL Java_android_os_Debug_getMemoryInfo__ILandroid_os_Debug_00024MemoryInfo_2(JNIEnv *env,jclass type,jint pid,jobject target){(void)type;return memory_info(env,current_process(pid),target);}
JNIEXPORT jlong JNICALL Java_android_os_Debug_getPss__(JNIEnv *env,jclass type){(void)env;(void)type;return 0;}
JNIEXPORT jlong JNICALL Java_android_os_Debug_getPss__I_3J_3J(JNIEnv *env,jclass type,jint pid,jlongArray details,jlongArray memtrack){
    (void)type;process_memory info;if(!current_process(pid)||!process_snapshot(&info))return 0;
    jlong values[]={(jlong)(info.unique/1024),0,(jlong)(info.resident/1024)},empty[4]={0};
    output_longs(env,details,values,3);output_longs(env,memtrack,empty,4);return 0;
}
JNIEXPORT jlong JNICALL Java_android_os_Debug_getRss__(JNIEnv *env,jclass type){(void)env;(void)type;uint64_t bytes;return resident_memory(&bytes)?(jlong)(bytes/1024):0;}
JNIEXPORT jlong JNICALL Java_android_os_Debug_getRss__I_3J(JNIEnv *env,jclass type,jint pid,jlongArray memtrack){
    (void)type;uint64_t bytes;if(!current_process(pid)||!resident_memory(&bytes))return 0;jlong empty[4]={0};output_longs(env,memtrack,empty,4);return (jlong)(bytes/1024);
}
