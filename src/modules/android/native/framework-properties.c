#include <jni.h>
#include "jni-lifetime.h"
#include <jvmti.h>
extern jvmtiEnv *klj_jni_vm_tooling(JNIEnv *);
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Raw AOSP SystemProperties shares the guest loader's property owner.
 * No second property dictionary or Java Build implementation is maintained. */
typedef void *(*lookup_fn)(const char *);
static lookup_fn lookup;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

static void error(JNIEnv *env,const char *type,const char *message) {
    jclass exception=(*env)->FindClass(env,type);
    if(exception){(*env)->ThrowNew(env,exception,message);(*env)->DeleteLocalRef(env,exception);}
}
static void *operation(JNIEnv *env,const char *name) {
    pthread_mutex_lock(&lock);
    if(!lookup)lookup=(lookup_fn)dlsym(RTLD_DEFAULT,"kl_shim_lookup");
    lookup_fn current=lookup;
    pthread_mutex_unlock(&lock);
    void *result=current?current(name):NULL;
    if(!result)error(env,"java/lang/UnsatisfiedLinkError","Android property owner is not loaded");
    return result;
}

/* Android's original ActivityManager reads these properties to report the
 * process heap allowance. HotSpot owns that allowance here, including -Xmx;
 * it has no separate manifest largeHeap limit. Publish the actual common limit
 * once, before Java or native application code can query the property area. */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *reserved) {
    (void)reserved;
    JNIEnv *env=NULL;
    if((*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)!=JNI_OK)return JNI_ERR;
    if(!klj_jni_vm_tooling(env))return JNI_ERR;
    jclass runtime=(*env)->FindClass(env,"java/lang/Runtime");
    if(!runtime)return JNI_ERR;
    jmethodID current=(*env)->GetStaticMethodID(env,runtime,"getRuntime","()Ljava/lang/Runtime;");
    jmethodID maximum=current?(*env)->GetMethodID(env,runtime,"maxMemory","()J"):NULL;
    if(!maximum){(*env)->DeleteLocalRef(env,runtime);return JNI_ERR;}
    jobject owner=(*env)->CallStaticObjectMethod(env,runtime,current);
    if((*env)->ExceptionCheck(env)||!owner){(*env)->DeleteLocalRef(env,runtime);return JNI_ERR;}
    jlong bytes=(*env)->CallLongMethod(env,owner,maximum);
    (*env)->DeleteLocalRef(env,owner);(*env)->DeleteLocalRef(env,runtime);
    if((*env)->ExceptionCheck(env))return JNI_ERR;
    int (*set)(const char *,const char *)=operation(env,"__system_property_set");
    if(!set)return JNI_ERR;
    jlong mib=bytes/(1024*1024);if(mib<1)mib=1;if(mib>INT_MAX)mib=INT_MAX;
    char value[32];snprintf(value,sizeof value,"%lldm",(long long)mib);
    if(set("dalvik.vm.heapsize",value)||set("dalvik.vm.heapgrowthlimit",value)){
        error(env,"java/lang/IllegalStateException","Android heap configuration could not be published");return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
static int read_key(JNIEnv *env,jstring key,char value[92]) {
    value[0]=0;
    if(!key){error(env,"java/lang/NullPointerException","Property name is null");return 0;}
    int (*get)(const char *,char *)=operation(env,"__system_property_get");
    if(!get)return 0;
    const char *name=(*env)->GetStringUTFChars(env,key,NULL);if(!name)return 0;
    int length=get(name,value);(*env)->ReleaseStringUTFChars(env,key,name);value[91]=0;
    return length>0;
}
static int read_handle(JNIEnv *env,jlong handle,char value[92]) {
    value[0]=0;const void *property=(const void *)(uintptr_t)handle;
    if(!property){error(env,"java/lang/IllegalArgumentException","Unknown Android property handle");return 0;}
    void (*read)(const void *,char *,char *)=operation(env,"__system_property_read");
    if(!read)return 0;
    read(property,NULL,value);value[91]=0;return value[0]!=0;
}
static jlong integer(const char *value,jlong fallback) {
    if(!value[0])return fallback;
    char *end;errno=0;long long result=strtoll(value,&end,0);
    return errno==ERANGE||end==value||*end?fallback:(jlong)result;
}
static jint small_integer(const char *value,jint fallback) {
    jlong result=integer(value,fallback);return result<INT_MIN||result>INT_MAX?fallback:(jint)result;
}
static jboolean boolean(const char *value,jboolean fallback) {
    if(!strcmp(value,"1")||!strcmp(value,"true")||!strcmp(value,"y")||!strcmp(value,"yes")||!strcmp(value,"on"))return JNI_TRUE;
    if(!strcmp(value,"0")||!strcmp(value,"false")||!strcmp(value,"n")||!strcmp(value,"no")||!strcmp(value,"off"))return JNI_FALSE;
    return fallback;
}
JNIEXPORT jstring JNICALL Java_android_os_SystemProperties_native_1get__Ljava_lang_String_2Ljava_lang_String_2(JNIEnv *env,jclass owner,jstring key,jstring fallback) {
    (void)owner;char value[92];int present=read_key(env,key,value);if((*env)->ExceptionCheck(env))return NULL;
    return present?(*env)->NewStringUTF(env,value):fallback?fallback:(*env)->NewStringUTF(env,"");
}
JNIEXPORT jint JNICALL Java_android_os_SystemProperties_native_1get_1int__Ljava_lang_String_2I(JNIEnv *env,jclass owner,jstring key,jint fallback) {
    (void)owner;char value[92];read_key(env,key,value);return small_integer(value,fallback);
}
JNIEXPORT jlong JNICALL Java_android_os_SystemProperties_native_1get_1long__Ljava_lang_String_2J(JNIEnv *env,jclass owner,jstring key,jlong fallback) {
    (void)owner;char value[92];read_key(env,key,value);return integer(value,fallback);
}
JNIEXPORT jboolean JNICALL Java_android_os_SystemProperties_native_1get_1boolean__Ljava_lang_String_2Z(JNIEnv *env,jclass owner,jstring key,jboolean fallback) {
    (void)owner;char value[92];read_key(env,key,value);return boolean(value,fallback);
}
JNIEXPORT jlong JNICALL Java_android_os_SystemProperties_native_1find(JNIEnv *env,jclass owner,jstring key) {
    (void)owner;if(!key){error(env,"java/lang/NullPointerException","Property name is null");return 0;}
    const void *(*find)(const char *)=operation(env,"__system_property_find");if(!find)return 0;
    const char *name=(*env)->GetStringUTFChars(env,key,NULL);if(!name)return 0;
    const void *property=find(name);(*env)->ReleaseStringUTFChars(env,key,name);if(!property)return 0;
    // The upstream owner returns process-lifetime prop_info pointers. AOSP's
    // Handle holds that borrowed pointer directly, exactly like a native guest.
    return (jlong)(uintptr_t)property;
}
JNIEXPORT jstring JNICALL Java_android_os_SystemProperties_native_1get__J(JNIEnv *env,jclass owner,jlong handle) {
    (void)owner;char value[92];read_handle(env,handle,value);return (*env)->ExceptionCheck(env)?NULL:(*env)->NewStringUTF(env,value);
}
JNIEXPORT jint JNICALL Java_android_os_SystemProperties_native_1get_1int__JI(JNIEnv *env,jclass owner,jlong handle,jint fallback) {
    (void)owner;char value[92];read_handle(env,handle,value);return small_integer(value,fallback);
}
JNIEXPORT jlong JNICALL Java_android_os_SystemProperties_native_1get_1long__JJ(JNIEnv *env,jclass owner,jlong handle,jlong fallback) {
    (void)owner;char value[92];read_handle(env,handle,value);return integer(value,fallback);
}
JNIEXPORT jboolean JNICALL Java_android_os_SystemProperties_native_1get_1boolean__JZ(JNIEnv *env,jclass owner,jlong handle,jboolean fallback) {
    (void)owner;char value[92];read_handle(env,handle,value);return boolean(value,fallback);
}
JNIEXPORT void JNICALL Java_android_os_SystemProperties_native_1set(JNIEnv *env,jclass owner,jstring key,jstring value) {
    (void)owner;(void)key;(void)value;
    error(env,"java/lang/SecurityException","Android system properties are read-only");
}
JNIEXPORT void JNICALL Java_android_os_SystemProperties_native_1add_1change_1callback(JNIEnv *env,jclass owner) {
    (void)env;(void)owner;/* Immutable host properties have no external change events. AOSP owns its listeners. */
}
JNIEXPORT void JNICALL Java_android_os_SystemProperties_native_1report_1sysprop_1change(JNIEnv *env,jclass owner) {
    jmethodID callback=(*env)->GetStaticMethodID(env,owner,"callChangeCallbacks","()V");
    if(callback)(*env)->CallStaticVoidMethod(env,owner,callback);
}
