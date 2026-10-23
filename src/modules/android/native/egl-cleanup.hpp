#pragma once
#include <jni.h>
namespace egl_cleanup {
inline void collect(JNIEnv *env,jthrowable &first){
 jthrowable next=env->ExceptionOccurred();if(next)env->ExceptionClear();
 if(!first){first=next;return;}
 if(first&&next&&!env->IsSameObject(first,next)){
  jclass type=env->FindClass("java/lang/Throwable");jmethodID add=type?env->GetMethodID(type,"addSuppressed","(Ljava/lang/Throwable;)V"):nullptr;if(add)env->CallVoidMethod(first,add,next);if(env->ExceptionCheck())env->ExceptionClear();if(type)env->DeleteLocalRef(type);
 }
 if(next)env->DeleteLocalRef(next);
}
inline void restore(JNIEnv *env,jthrowable first){
 collect(env,first);if(first){env->Throw(first);env->DeleteLocalRef(first);}
}
template<class Finish,class Detach,class Destroy,class Retire>
unsigned surface(JNIEnv *env,Finish finish,Detach detach,Destroy destroy,Retire retire){
 // A completed GPU error is still retired work. Preserve its exception while
 // releasing the surface; only an actually unretired producer blocks cleanup.
 jthrowable first=env->ExceptionOccurred();if(first)env->ExceptionClear();
 const bool drained=finish();collect(env,first);if(!drained){restore(env,first);return 0;}
 unsigned result=destroy();if(!result||env->ExceptionCheck()){restore(env,first);return 0;}
 if(!detach()||env->ExceptionCheck()){restore(env,first);return 0;}
 retire();restore(env,first);return result;
}
}
