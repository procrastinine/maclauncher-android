#pragma once
#include <jni.h>
#include <utility>
extern "C" void* klj_framework_acquire_window(JNIEnv*,jobject);
extern "C" void klj_framework_release_window(void*);
namespace android {
class HostNativeWindow {
 void* pointer=nullptr;
 public: HostNativeWindow()=default;explicit HostNativeWindow(void*p):pointer(p){}~HostNativeWindow(){if(pointer)klj_framework_release_window(pointer);}
 HostNativeWindow(HostNativeWindow&& other):pointer(std::exchange(other.pointer,nullptr)){}
 HostNativeWindow&operator=(HostNativeWindow&& other){if(pointer)klj_framework_release_window(pointer);pointer=std::exchange(other.pointer,nullptr);return *this;}
 void* get()const{return pointer;}bool operator==(std::nullptr_t)const{return pointer==nullptr;}
};
inline HostNativeWindow android_view_Surface_getNativeWindow(JNIEnv*e,jobject surface){return HostNativeWindow(klj_framework_acquire_window(e,surface));}
inline HostNativeWindow android_graphics_SurfaceTexture_getNativeWindow(JNIEnv*e,jobject texture){
 jclass type=e->FindClass("android/view/Surface");if(!type)return {};
 jmethodID ctor=e->GetMethodID(type,"<init>","(Landroid/graphics/SurfaceTexture;)V");
 jmethodID release=e->GetMethodID(type,"release","()V");
 jobject surface=ctor&&release?e->NewObject(type,ctor,texture):nullptr;
 HostNativeWindow window;
 if(surface&&!e->ExceptionCheck()){
  window=android_view_Surface_getNativeWindow(e,surface);
  if(!e->ExceptionCheck())e->CallVoidMethod(surface,release);
 }
 if(surface)e->DeleteLocalRef(surface);e->DeleteLocalRef(type);return window;
}
int egl_get_init_count(void* display);
}
