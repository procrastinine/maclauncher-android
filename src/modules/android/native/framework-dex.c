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

/* Keep DexFile's original native declarations and ordinary VM registration
 * semantics. JNI exports delegate to the same JVM DEX owner; re-registration
 * and UnregisterNatives use the VM table and normal native symbol resolution. */
static jvalue dex_operation(JNIEnv *env,const char *name,const char *signature,jvalue *arguments,char result){
    jvalue value={0};
    jclass owner=(*env)->FindClass(env,"maclauncher/android/framework/HostDexFiles");
    if(!owner)return value;
    jmethodID method=(*env)->GetStaticMethodID(env,owner,name,signature);
    if(method){
        if(result=='Z')value.z=(*env)->CallStaticBooleanMethodA(env,owner,method,arguments);
        else if(result=='V')(*env)->CallStaticVoidMethodA(env,owner,method,arguments);
        else value.l=(*env)->CallStaticObjectMethodA(env,owner,method,arguments);
    }
    (*env)->DeleteLocalRef(env,owner);return value;
}
JNIEXPORT jobject JNICALL Java_dalvik_system_DexFile_openDexFileNative(JNIEnv *env,jclass type,jstring source,jstring output,jint flags,jobject loader,jobjectArray elements){
    (void)type;jvalue args[]={{.l=source},{.l=output},{.i=flags},{.l=loader},{.l=elements}};
    return dex_operation(env,"openDexFileNative","(Ljava/lang/String;Ljava/lang/String;ILjava/lang/ClassLoader;[Ljava/lang/Object;)Ljava/lang/Object;",args,'L').l;
}
JNIEXPORT jobject JNICALL Java_dalvik_system_DexFile_openInMemoryDexFilesNative(JNIEnv *env,jclass type,jobjectArray buffers,jobjectArray arrays,jintArray starts,jintArray ends,jobject loader,jobjectArray elements){
    (void)type;jvalue args[]={{.l=buffers},{.l=arrays},{.l=starts},{.l=ends},{.l=loader},{.l=elements}};
    return dex_operation(env,"openInMemoryDexFilesNative","([Ljava/nio/ByteBuffer;[[B[I[ILjava/lang/ClassLoader;[Ljava/lang/Object;)Ljava/lang/Object;",args,'L').l;
}
JNIEXPORT jboolean JNICALL Java_dalvik_system_DexFile_closeDexFile(JNIEnv *env,jclass type,jobject cookie){
    (void)type;jvalue args[]={{.l=cookie}};return dex_operation(env,"closeDexFile","(Ljava/lang/Object;)Z",args,'Z').z;
}
JNIEXPORT jclass JNICALL Java_dalvik_system_DexFile_defineClassNative(JNIEnv *env,jclass type,jstring name,jobject loader,jobject cookie,jobject owner){
    (void)type;jvalue args[]={{.l=name},{.l=loader},{.l=cookie},{.l=owner}};
    return dex_operation(env,"defineClassNative","(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/Object;Ldalvik/system/DexFile;)Ljava/lang/Class;",args,'L').l;
}
JNIEXPORT jobjectArray JNICALL Java_dalvik_system_DexFile_getClassNameList(JNIEnv *env,jclass type,jobject cookie){
    (void)type;jvalue args[]={{.l=cookie}};return dex_operation(env,"getClassNameList","(Ljava/lang/Object;)[Ljava/lang/String;",args,'L').l;
}
JNIEXPORT jboolean JNICALL Java_dalvik_system_DexFile_isBackedByOatFile(JNIEnv *env,jclass type,jobject cookie){
    (void)type;jvalue args[]={{.l=cookie}};return dex_operation(env,"isBackedByOatFile","(Ljava/lang/Object;)Z",args,'Z').z;
}
JNIEXPORT void JNICALL Java_dalvik_system_DexFile_verifyInBackgroundNative(JNIEnv *env,jclass type,jobject cookie,jobject loader){
    (void)type;jvalue args[]={{.l=cookie},{.l=loader}};dex_operation(env,"verifyInBackgroundNative","(Ljava/lang/Object;Ljava/lang/ClassLoader;)V",args,'V');
}
JNIEXPORT void JNICALL Java_dalvik_system_DexFile_setTrusted(JNIEnv *env,jclass type,jobject cookie){
    (void)type;jvalue args[]={{.l=cookie}};dex_operation(env,"setTrusted","(Ljava/lang/Object;)V",args,'V');
}
JNIEXPORT jboolean JNICALL Java_dalvik_system_DexFile_isReadOnlyJavaDclEnforced(JNIEnv *env,jclass type){
    (void)type;return dex_operation(env,"isReadOnlyJavaDclEnforced","()Z",NULL,'Z').z;
}
