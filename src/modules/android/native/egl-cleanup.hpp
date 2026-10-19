#pragma once
#include <jni.h>
namespace egl_cleanup {
inline void restore(JNIEnv *env,jthrowable first){
 jthrowable next=env->ExceptionOccurred();if(next)env->ExceptionClear();
 if(first&&next&&!env->IsSameObject(first,next)){
  jclass type=env->FindClass("java/lang/Throwable");jmethodID add=type?env->GetMethodID(type,"addSuppressed","(Ljava/lang/Throwable;)V"):nullptr;if(add)env->CallVoidMethod(first,add,next);if(env->ExceptionCheck())env->ExceptionClear();if(type)env->DeleteLocalRef(type);
 }
 if(first)env->Throw(first);else if(next)env->Throw(next);
 if(next)env->DeleteLocalRef(next);if(first)env->DeleteLocalRef(first);
}
template<class Finish,class Detach,class Destroy,class Retire>
unsigned surface(JNIEnv *env,Finish finish,Detach detach,Destroy destroy,Retire retire){
 // A guest's first JNI exception must survive cleanup, but must not prevent
 // the owned surface from draining and releasing successfully.
 jthrowable first=env->ExceptionOccurred();if(first)env->ExceptionClear();
 finish();if(env->ExceptionCheck()){restore(env,first);return 0;}
 unsigned result=destroy();if(!result||env->ExceptionCheck()){restore(env,first);return 0;}
 if(!detach()||env->ExceptionCheck()){restore(env,first);return 0;}
 retire();restore(env,first);return result;
}
}
