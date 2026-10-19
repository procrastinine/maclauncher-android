#include <jni.h>
#include <stdlib.h>
#include <string.h>
/* ClassLoaderFactory has already constructed the original BaseDexClassLoader,
 * including its library search path, before this JNI call. AOSP libnativeloader
 * also returns success here on non-Android hosts: Bionic linker namespaces are
 * an Android target operation. The JVM loader and GuestNativeLoader remain the
 * sole class and ELF owners; creating another namespace registry would split
 * their lifetime and JNI identity. Filesystem isolation remains process policy.
 */
JNIEXPORT jstring JNICALL Java_com_android_internal_os_ClassLoaderFactory_createClassloaderNamespace(
    JNIEnv *env,jclass type,jobject loader,jint target_sdk,jstring search,jstring permitted,
    jboolean shared,jstring dex_path,jstring sonames){
    (void)env;(void)type;(void)loader;(void)target_sdk;(void)search;
    (void)permitted;(void)shared;(void)dex_path;(void)sonames;
    return NULL;
}
/* ART's DexFile definition boundary resolves an existing class before defining
 * it. Use the JVM loader's actual table and loading lock, including calls made
 * directly through DexFile or with Android slash-form names. No second cache or
 * alternate loader may split the class's identity. */
JNIEXPORT jclass JNICALL Java_maclauncher_android_framework_HostDexFiles_defineBytecode(JNIEnv* env,jclass unused,jstring name,jobject loader,jbyteArray bytes){
    (void)unused;
    const char* binary=(*env)->GetStringUTFChars(env,name,NULL);if(!binary)return NULL;
    char *dotted=strdup(binary);jclass result=NULL,type=NULL;jstring requested=NULL;jobject lock=NULL;int entered=0;
    if(!dotted){type=(*env)->FindClass(env,"java/lang/OutOfMemoryError");if(type)(*env)->ThrowNew(env,type,"DEX class-name allocation failed");goto done;}
    for(char *at=dotted;*at;at++)if(*at=='/')*at='.';
    requested=(*env)->NewStringUTF(env,dotted);if(!requested)goto done;
    for(char *at=dotted;*at;at++)if(*at=='.')*at='/';
    type=(*env)->FindClass(env,"java/lang/ClassLoader");if(!type)goto done;
    jmethodID loading_lock=(*env)->GetMethodID(env,type,"getClassLoadingLock","(Ljava/lang/String;)Ljava/lang/Object;");if(!loading_lock)goto done;
    jmethodID loaded=(*env)->GetMethodID(env,type,"findLoadedClass","(Ljava/lang/String;)Ljava/lang/Class;");if(!loaded)goto done;
    lock=(*env)->CallObjectMethod(env,loader,loading_lock,requested);if((*env)->ExceptionCheck(env)||!lock)goto done;
    if((*env)->MonitorEnter(env,lock)!=JNI_OK)goto done;entered=1;
    result=(*env)->CallObjectMethod(env,loader,loaded,requested);if((*env)->ExceptionCheck(env)||result)goto done;
    jsize length=(*env)->GetArrayLength(env,bytes);jbyte* data=(*env)->GetByteArrayElements(env,bytes,NULL);
    if(data){result=(*env)->DefineClass(env,dotted,loader,data,length);(*env)->ReleaseByteArrayElements(env,bytes,data,JNI_ABORT);}
done:
    if(entered)(*env)->MonitorExit(env,lock);
    if(lock)(*env)->DeleteLocalRef(env,lock);if(type)(*env)->DeleteLocalRef(env,type);if(requested)(*env)->DeleteLocalRef(env,requested);
    free(dotted);(*env)->ReleaseStringUTFChars(env,name,binary);return result;
}
