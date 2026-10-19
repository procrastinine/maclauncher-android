#include <jni.h>
#include "bridge.h"
#include "../../native/memory-info.h"
#include <sys/sysinfo.h>
#include <android/dlext_private.h>
#define host klj_art_host
JNIEXPORT void JNICALL Java_maclauncher_android_ArtPlatform_libraryCompatibility(JNIEnv *env,jclass owner,jint targetSdk,jboolean enabled){
 (void)owner;
 // The original ART VMRuntime method also updates libdl's target-SDK state.
 jclass type=(*env)->FindClass(env,"dalvik/system/VMRuntime");if(!type)return;
 jmethodID get=(*env)->GetStaticMethodID(env,type,"getRuntime","()Ldalvik/system/VMRuntime;");
 jmethodID set=get?(*env)->GetMethodID(env,type,"setTargetSdkVersion","(I)V"):NULL;
 jobject runtime=set?(*env)->CallStaticObjectMethod(env,type,get):NULL;
 if(runtime){(*env)->CallVoidMethod(env,runtime,set,targetSdk);(*env)->DeleteLocalRef(env,runtime);}
 (*env)->DeleteLocalRef(env,type);
 if(!(*env)->ExceptionCheck(env))android_set_16kb_appcompat_mode(enabled==JNI_TRUE);
}
JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostProcessIdentity_physicalPidNative(JNIEnv *env,jclass owner){
 (void)owner;int64_t result=host(11,NULL,0);
 if(result<=0||result>INT32_MAX){jclass error=(*env)->FindClass(env,"java/lang/IllegalStateException");if(error)(*env)->ThrowNew(env,error,"Unable to read physical process identity");return 0;}
 return (jint)result;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_Clock_bootTimeNanos(JNIEnv *env,jclass owner) {
    (void)owner;uint64_t result=0;int64_t status=host(1,&result,sizeof(result));
    if(status){jclass error=(*env)->FindClass(env,"java/lang/IllegalStateException");if(error)(*env)->ThrowNew(env,error,"Unable to read elapsed realtime");return 0;}
    return (jlong)result;
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_Bridge_memoryInfo(JNIEnv *env,jclass owner,jlongArray output){
 (void)owner;
 if(!output||(*env)->GetArrayLength(env,output)!=4){jclass error=(*env)->FindClass(env,"java/lang/IllegalArgumentException");if(error)(*env)->ThrowNew(env,error,"Memory info requires four values");return;}
 struct sysinfo info;
 if(sysinfo(&info)){jclass error=(*env)->FindClass(env,"java/lang/IllegalStateException");if(error)(*env)->ThrowNew(env,error,"Unable to read native memory information");return;}
 // Original Bionic/AIM owns sysinfo and /proc/meminfo in this process.
 // Loading the other VM's libc would create a competing ABI owner.
 jlong values[4];klj_memory_info_values((uint64_t)info.totalram*info.mem_unit,(uint64_t)info.freeram*info.mem_unit,80,values);
 (*env)->SetLongArrayRegion(env,output,0,4,values);
}
#include "../../native/vulkan-capabilities.h"
#include <pthread.h>
static pthread_once_t graphics_once=PTHREAD_ONCE_INIT;
static KljVulkanCapabilities graphics_capabilities={0,-1};
static void graphics_query(void){graphics_capabilities=klj_vulkan_query_capabilities(vkGetInstanceProcAddr);}
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_Graphics_vulkanVersion(JNIEnv *env,jclass owner){
 (void)env;(void)owner;pthread_once(&graphics_once,graphics_query);return graphics_capabilities.version;
}
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_Graphics_vulkanHardwareLevel(JNIEnv *env,jclass owner){
 (void)env;(void)owner;pthread_once(&graphics_once,graphics_query);return graphics_capabilities.level;
}
