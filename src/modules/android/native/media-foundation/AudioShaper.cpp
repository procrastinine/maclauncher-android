#include "audio-shaper.h"
#include <atomic>
#include <mutex>
#include <new>
#include <media/VolumeShaper.h>
#include "android_media_VolumeShaper.h"

namespace {
using android::media::VolumeHandler;
using android::media::VolumeShaper;
struct Envelope {
    VolumeHandler handler;
    std::atomic<bool> configured{false};
    explicit Envelope(unsigned rate):handler(rate){}
};
android::VolumeShaperHelper::fields_t fields{};
std::once_flag initialized;
bool initialize(JNIEnv *env){std::call_once(initialized,[&]{fields.init(env);});return !env->ExceptionCheck();}
}
extern "C" void *klj_audio_shaper_create(unsigned rate){return new(std::nothrow) Envelope(rate);}
extern "C" void klj_audio_shaper_destroy(void *value){delete static_cast<Envelope*>(value);}
extern "C" float klj_audio_shaper_gain(void *value,uint64_t frame){
    auto *owner=static_cast<Envelope*>(value);
    return owner&&owner->configured.load(std::memory_order_acquire)?owner->handler.getVolume(static_cast<int64_t>(frame)).first:1.f;
}
extern "C" int klj_audio_shaper_apply(JNIEnv *env,void *value,jobject config,jobject operation){
    auto *owner=static_cast<Envelope*>(value);if(!owner)return -38;
    if(!config||!operation)return android::BAD_VALUE;if(!initialize(env))return android::BAD_VALUE;
    auto configuration=android::VolumeShaperHelper::convertJobjectToConfiguration(env,fields,config);
    if(env->ExceptionCheck()||!configuration)return android::BAD_VALUE;
    auto action=android::VolumeShaperHelper::convertJobjectToOperation(env,fields,operation);
    if(env->ExceptionCheck()||!action)return android::BAD_VALUE;
    owner->handler.setIdIfNecessary(configuration);
    int result=owner->handler.applyVolumeShaper(configuration,action);
    if(result>=0)owner->configured.store(true,std::memory_order_release);
    // Java's platform-stable status is Linux ENOSYS, independently of Darwin.
    return result==android::INVALID_OPERATION?-38:result;
}
extern "C" jobject klj_audio_shaper_state(JNIEnv *env,void *value,int id){
    auto *owner=static_cast<Envelope*>(value);if(!owner||!initialize(env))return nullptr;
    auto state=owner->handler.getVolumeShaperState(id);
    return state?android::VolumeShaperHelper::convertStateToJobject(env,fields,state):nullptr;
}
