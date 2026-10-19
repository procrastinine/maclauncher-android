#include "Access.h"
#include <jni.h>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <cerrno>
// Logical object context for the existing service-directory owner. This does
// not relabel the process or change its authenticated KeyStore security role.
extern "C" int klj_service_directory_context(char **context){
    if(!context){errno=EINVAL;return -1;}*context=strdup("u:r:servicemanager:s0");return *context?0:-1;
}
namespace {
std::mutex accessMutex;
android::Access& policy(){static android::Access value;return value;}
jboolean allowed(JNIEnv* env,jstring name,bool add){
    if(!name)return JNI_FALSE;const char* value=env->GetStringUTFChars(name,nullptr);if(!value)return JNI_FALSE;
    bool result;{
        std::lock_guard lock(accessMutex);auto& access=policy();auto caller=access.getCallingContext();
        result=add?access.canAdd(caller,value):access.canFind(caller,value);
    }
    env->ReleaseStringUTFChars(name,value);return result?JNI_TRUE:JNI_FALSE;
}
}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_framework_HostServices_canFindNative(JNIEnv* env,jclass,jstring name){return allowed(env,name,false);}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_framework_HostServices_canAddNative(JNIEnv* env,jclass,jstring name){return allowed(env,name,true);}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_framework_HostServices_canListNative(JNIEnv*,jclass){
    std::lock_guard lock(accessMutex);auto& access=policy();return access.canList(access.getCallingContext());
}
