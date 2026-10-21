#include <jni.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern jlong Java_android_os_Process_getTotalMemory(JNIEnv *,jclass);
extern jlong Java_android_os_Process_getFreeMemory(JNIEnv *,jclass);
static uint64_t (*budget)(void),(*available)(void);
static const char *(*guest_path)(const char *,char *,size_t);
JNIEXPORT void JNICALL Java_MemoryBudgetProbe_bind(JNIEnv *env,jclass owner,jboolean source){
    (void)owner;
    budget=dlsym(RTLD_DEFAULT,"kl_mem_budget_bytes");available=dlsym(RTLD_DEFAULT,"kl_mem_available_bytes");
    guest_path=dlsym(RTLD_DEFAULT,"kl_guest_path");assert(budget&&available&&guest_path);
    if(source){jclass process=(*env)->FindClass(env,"android/os/Process");if(!process)return;
        JNINativeMethod methods[]={{"getTotalMemory","()J",(void *)Java_android_os_Process_getTotalMemory},{"getFreeMemory","()J",(void *)Java_android_os_Process_getFreeMemory}};
        assert(!(*env)->RegisterNatives(env,process,methods,2));(*env)->DeleteLocalRef(env,process);}
}
JNIEXPORT jlongArray JNICALL Java_MemoryBudgetProbe_provider(JNIEnv *env,jclass owner){
    (void)owner;uint64_t physical=0;size_t size=sizeof physical;assert(!sysctlbyname("hw.memsize",&physical,&size,NULL,0));
    jlong values[]={budget(),available(),physical};jlongArray result=(*env)->NewLongArray(env,3);
    if(result)(*env)->SetLongArrayRegion(env,result,0,3,values);return result;
}
JNIEXPORT jlongArray JNICALL Java_MemoryBudgetProbe_procMemory(JNIEnv *env,jclass owner){
    (void)owner;char mapped[4096];const char *file=guest_path("/proc/meminfo",mapped,sizeof mapped);assert(file);
    FILE *input=fopen(file,"rb");assert(input);char line[256];jlong values[]={-1,-1,-1};
    while(fgets(line,sizeof line,input)){unsigned long long value;
        if(sscanf(line,"MemTotal: %llu kB",&value)==1)values[0]=(jlong)value;
        else if(sscanf(line,"MemFree: %llu kB",&value)==1)values[1]=(jlong)value;
        else if(sscanf(line,"MemAvailable: %llu kB",&value)==1)values[2]=(jlong)value;}
    fclose(input);jlongArray result=(*env)->NewLongArray(env,3);if(result)(*env)->SetLongArrayRegion(env,result,0,3,values);return result;
}
JNIEXPORT jlong JNICALL Java_MemoryBudgetProbe_allocate(JNIEnv *env,jclass owner,jlong bytes){
    (void)env;(void)owner;void *memory=mmap(NULL,(size_t)bytes,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(memory==MAP_FAILED)return 0;memset(memory,0x6a,(size_t)bytes);return (jlong)(uintptr_t)memory;
}
JNIEXPORT void JNICALL Java_MemoryBudgetProbe_release(JNIEnv *env,jclass owner,jlong address,jlong bytes){
    (void)env;(void)owner;assert(!munmap((void *)(uintptr_t)address,(size_t)bytes));
}
