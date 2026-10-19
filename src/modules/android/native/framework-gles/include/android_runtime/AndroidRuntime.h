#pragma once
#include <jni.h>
namespace android { class AndroidRuntime {public: static int registerNativeMethods(JNIEnv* env,const char* name,const JNINativeMethod* methods,int count){jclass type=env->FindClass(name);if(!type)return -1;int result=env->RegisterNatives(type,methods,count);env->DeleteLocalRef(type);return result;}}; }
