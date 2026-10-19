#pragma once
#include <android/bitmap.h>
#include <nativehelper/JNIPlatformHelp.h>

extern "C" void* kl_shim_lookup(const char*);

namespace android {
// GLUtils borrows the same original Bitmap through its NDK lock owner. That
// owner also provides the publisher's BGRA -> Android RGBA view when required.
// No new bitmap, pixel allocation owner, or Java framework substitute exists.
class HostGraphicsBitmap {
    JNIEnv* env;
    jobject object;
    AndroidBitmapInfo info{};
    void* pixels=nullptr;
    bool valid=false,locked=false;
    template<class Fn> Fn operation(const char* name) {
        auto result=reinterpret_cast<Fn>(kl_shim_lookup(name));
        if(!result&&!env->ExceptionCheck())jniThrowException(env,"java/lang/UnsatisfiedLinkError",name);
        return result;
    }
public:
    HostGraphicsBitmap(JNIEnv* e,jobject value):env(e),object(value) {
        auto get=operation<decltype(&AndroidBitmap_getInfo)>("AndroidBitmap_getInfo");
        valid=get&&get(env,object,&info)==ANDROID_BITMAP_RESULT_SUCCESS;
    }
    ~HostGraphicsBitmap(){
        if(locked){auto unlock=operation<decltype(&AndroidBitmap_unlockPixels)>("AndroidBitmap_unlockPixels");if(unlock)unlock(env,object);}
    }
    HostGraphicsBitmap(const HostGraphicsBitmap&)=delete;
    bool isValid()const{return valid;}
    const AndroidBitmapInfo& getInfo()const{return info;}
    void* getPixels(){
        if(valid&&!locked){
            auto lock=operation<decltype(&AndroidBitmap_lockPixels)>("AndroidBitmap_lockPixels");
            locked=lock&&lock(env,object,&pixels)==ANDROID_BITMAP_RESULT_SUCCESS;
        }
        return locked?pixels:nullptr;
    }
};
}
