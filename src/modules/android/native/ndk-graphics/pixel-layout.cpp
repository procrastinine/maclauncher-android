#include <android/bitmap.h>
#include <android/graphics/bitmap.h>
#include <hwui/Bitmap.h>
#include <TypeCast.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <new>
#include <string.h>

extern "C" int AndroidBitmap_lockPixels_original(JNIEnv*,jobject,void**);
extern "C" int AndroidBitmap_unlockPixels_original(JNIEnv*,jobject);

namespace {
// Original NDK locks retain the original Bitmap. A lease owns only a temporary
// pixel-layout view and a weak Java identity for cleanup after Bitmap.recycle().
struct Lease {std::mutex mutex;std::vector<uint8_t> rgba;jweak object=nullptr;size_t locks=0;};
std::mutex leasesMutex;
std::unordered_map<ABitmap*,std::shared_ptr<Lease>> leases;
void swapRedBlue(const void* source,void* destination,const android::Bitmap& bitmap) {
    const size_t row=bitmap.rowBytes();
    for(int y=0;y<bitmap.height();y++){
        const auto* src=static_cast<const uint8_t*>(source)+y*row;
        auto* dst=static_cast<uint8_t*>(destination)+y*row;
        for(int x=0;x<bitmap.width();x++){
            uint32_t pixel;memcpy(&pixel,src+x*4,4);
            pixel=(pixel&0xff00ff00u)|((pixel&0xffu)<<16)|((pixel>>16)&0xffu);
            memcpy(dst+x*4,&pixel,4);
        }
    }
}
}

extern "C" int AndroidBitmap_lockPixels(JNIEnv* env,jobject object,void** output) {
    if(!env||!object)return AndroidBitmap_lockPixels_original(env,object,output);
    android::graphics::Bitmap handle(env,object);
    void* pixels=nullptr;
    const int result=AndroidBitmap_lockPixels_original(env,object,&pixels);
    if(result)return result;
    auto* bitmap=android::TypeCast::toBitmap(handle.get());
    try {
        for(;;){
            std::shared_ptr<Lease> lease;
            {
                std::lock_guard registry(leasesMutex);
                auto entry=leases.find(handle.get());
                if(entry==leases.end()){
                    lease=std::make_shared<Lease>();
                    lease->object=env->NewWeakGlobalRef(object);
                    if(!lease->object)throw std::bad_alloc();
                    try{leases.emplace(handle.get(),lease);}
                    catch(...){env->DeleteWeakGlobalRef(lease->object);throw;}
                }else lease=entry->second;
            }
            std::lock_guard operation(lease->mutex);
            {
                std::lock_guard registry(leasesMutex);
                auto entry=leases.find(handle.get());
                // A final unlock can retire a lease before this operation gets
                // its mutex. Retry with the current lease, retaining our bitmap.
                if(entry==leases.end()||entry->second!=lease)continue;
            }
            try{
                if(!lease->locks&&bitmap->colorType()==kBGRA_8888_SkColorType){
                    lease->rgba.resize(bitmap->rowBytes()*bitmap->height());
                    swapRedBlue(pixels,lease->rgba.data(),*bitmap);
                }
            }catch(...){
                std::lock_guard registry(leasesMutex);
                leases.erase(handle.get());env->DeleteWeakGlobalRef(lease->object);throw;
            }
            lease->locks++;
            if(output)*output=lease->rgba.empty()?pixels:lease->rgba.data();
            return result;
        }
    }catch(const std::bad_alloc&){
        ABitmap_releaseRef(handle.get()); // balance the successful original lock
        return ANDROID_BITMAP_RESULT_ALLOCATION_FAILED;
    }
}

extern "C" int AndroidBitmap_unlockPixels(JNIEnv* env,jobject object) {
    if(!env||!object)return AndroidBitmap_unlockPixels_original(env,object);
    android::graphics::Bitmap handle(env,object);
    ABitmap* key=handle.get();std::shared_ptr<Lease> lease;
    {
        std::lock_guard registry(leasesMutex);
        auto entry=leases.find(key);
        if(entry==leases.end()&&!key){
            for(auto candidate=leases.begin();candidate!=leases.end();++candidate)
                if(env->IsSameObject(candidate->second->object,object)){entry=candidate;key=entry->first;break;}
        }
        if(entry!=leases.end())lease=entry->second;
    }
    if(!lease)return AndroidBitmap_unlockPixels_original(env,object);
    std::lock_guard operation(lease->mutex);
    {
        std::lock_guard registry(leasesMutex);
        auto entry=leases.find(key);
        if(entry==leases.end()||entry->second!=lease)return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
    }
    if(!--lease->locks){
        auto* bitmap=android::TypeCast::toBitmap(key);
        if(!lease->rgba.empty())swapRedBlue(lease->rgba.data(),bitmap->pixels(),*bitmap);
        std::lock_guard registry(leasesMutex);
        leases.erase(key);env->DeleteWeakGlobalRef(lease->object);
    }
    if(handle.isValid())return AndroidBitmap_unlockPixels_original(env,object);
    // recycle() clears the Java wrapper, but the successful native lock still
    // owns its exact ABitmap reference. Release it even though the original JNI
    // wrapper can no longer retrieve it; retain the original invalid result.
    ABitmap_notifyPixelsChanged(key);ABitmap_releaseRef(key);
    return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
}
