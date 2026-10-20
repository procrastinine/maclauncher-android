#include "media-surface.h"
#include "surface-texture.hpp"
#include "video-pixels.hpp"
#include "presentation.h"
#include "native-clock/clock-domain.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <ctime>
#include <climits>
extern "C" void *klj_surface_native_window(JNIEnv*,jobject);
extern "C" bool klj_surface_retain_window(void*);
extern "C" void klj_surface_release_window(void*);
extern "C" uint64_t klj_surface_consumer(void*);
extern "C" void *klj_surface_acquire_lease(uint64_t,void**,int*,int*);
extern "C" void klj_surface_release_lease(void*);
extern "C" bool klj_texture_publish_video(JNIEnv*,uint64_t,CVPixelBufferRef,int64_t,void(*)(void*,int64_t),void*);
namespace {
constexpr int unsupported=-10002,invalid=-10004,failed=-10000;
JavaVM*vm=nullptr;jclass handles=nullptr,surfaces=nullptr;jmethodID retainHandle,releaseHandle,targetKind,submitPixels;
std::once_flag initialized;
struct Attached {JNIEnv*env=nullptr;bool detach=false;Attached(){if(vm&&vm->GetEnv((void**)&env,JNI_VERSION_1_8)==JNI_EDETACHED)detach=vm->AttachCurrentThreadAsDaemon((void**)&env,nullptr)==JNI_OK;}~Attached(){if(detach)vm->DetachCurrentThread();}};
JNIEnv* environment(){thread_local Attached thread;return thread.env;}
int64_t now(){timespec value;klj_clock_read(1,&value);return int64_t(value.tv_sec)*1000000000+value.tv_nsec;}
struct Surface {
    std::atomic<unsigned> refs{1};void*window;uint64_t lease;jlong handle;int kind;
    ~Surface(){Attached thread;if(thread.env){auto*env=thread.env;jthrowable pending=env->ExceptionOccurred();if(pending)env->ExceptionClear();env->CallStaticVoidMethod(handles,releaseHandle,handle);if(pending){if(env->ExceptionCheck())env->ExceptionClear();env->Throw(pending);env->DeleteLocalRef(pending);}}klj_surface_release_window(window);}
    static void retain(void*p){static_cast<Surface*>(p)->refs.fetch_add(1,std::memory_order_relaxed);}
    static void release(void*p){auto*s=static_cast<Surface*>(p);if(s->refs.fetch_sub(1,std::memory_order_acq_rel)==1)delete s;}
    int deliver(CVPixelBufferRef pixels,int64_t time,void(*completed)(void*,int64_t),void*context){@autoreleasepool{
        auto*env=environment();if(!env)return failed;
        if(kind==1)return klj_texture_publish_video(env,lease,pixels,time,completed,context)?0:failed;
        if(kind==2){
            size_t width=CVPixelBufferGetWidth(pixels),height=CVPixelBufferGetHeight(pixels),count=width*height;if(count>INT_MAX)return invalid;
            std::vector<jint> values(count);if(!klj_video_argb(pixels,values.data(),width*4))return failed;
            auto array=env->NewIntArray((jsize)count);if(!array)return failed;env->SetIntArrayRegion(array,0,(jsize)count,values.data());
            bool okay=!env->ExceptionCheck()&&env->CallStaticBooleanMethod(surfaces,submitPixels,handle,array,(jint)width,(jint)height,time);
            if(env->ExceptionCheck())okay=false;env->DeleteLocalRef(array);if(!okay)return failed;if(completed)completed(context,now());return 0;
        }
        void*presenter=nullptr;int width=0,height=0;void*active=klj_surface_acquire_lease(lease,&presenter,&width,&height);if(!active)return invalid;
        struct Active {void*window;~Active(){klj_surface_release_lease(window);}} activeOwner{active};if(!presenter)return unsupported;
        auto device=MTLCreateSystemDefaultDevice();auto queue=(__bridge id<MTLCommandQueue>)klj_metal_projection_queue(env,presenter,(__bridge void*)device);if(!queue||env->ExceptionCheck())return failed;
        auto commands=[queue commandBuffer];commands.label=@"Android decoded video";auto texture=klj_video_texture(pixels,device,commands);if(!texture)return failed;
        // Each decoded frame owns this immutable conversion texture; retain it
        // for idle PixelCopy without another per-frame GPU image copy.
        int result=klj_metal_project_retained(env,presenter,nullptr,(__bridge void*)commands,(__bridge void*)texture,
            (__bridge_retained void*)texture,[](void*value){id owner=(__bridge_transfer id)value;(void)owner;});if(result||env->ExceptionCheck())return failed;
        if(completed)[commands addCompletedHandler:^(id<MTLCommandBuffer> buffer){completed(context,buffer.status==MTLCommandBufferStatusCompleted?now():0);}];
        [commands commit];return 0;
    }}
    static int present(void*p,CVPixelBufferRef pixels,int64_t mediaUs,int64_t requested,void(*completed)(void*,int64_t),void*context){
        auto*self=static_cast<Surface*>(p);int64_t current=now();
        if(requested>current){retain(self);CVPixelBufferRetain(pixels);uint64_t delay=(uint64_t)(requested-current);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,delay),dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
                int result=failed;try{result=self->deliver(pixels,mediaUs*1000,completed,context);}catch(...){result=failed;}
                if(result&&completed)completed(context,0);CVPixelBufferRelease(pixels);release(self);
            });return 0;}
        try{return self->deliver(pixels,mediaUs*1000,completed,context);}catch(...){return failed;}
    }
};
int acquire(void*window,KLJMediaSurface*out){
    if(!window||!out)return invalid;if(!klj_surface_retain_window(window))return unsupported;
    auto*env=environment();if(!env){klj_surface_release_window(window);return failed;}
    jlong handle=(jlong)klj_surface_consumer(window);if(!handle){klj_surface_release_window(window);return unsupported;}
    handle=env->CallStaticLongMethod(handles,retainHandle,handle);if(env->ExceptionCheck()||!handle){klj_surface_release_window(window);return invalid;}
    int kind=env->CallStaticIntMethod(surfaces,targetKind,handle);
    if(env->ExceptionCheck()){env->CallStaticVoidMethod(handles,releaseHandle,handle);klj_surface_release_window(window);return failed;}
    auto*value=new(std::nothrow) Surface;if(!value){env->CallStaticVoidMethod(handles,releaseHandle,handle);klj_surface_release_window(window);return failed;}
    value->window=window;value->handle=handle;value->lease=klj_surface_lease(window);value->kind=kind;
    *out={value,Surface::retain,Surface::release,Surface::present};return 0;
}
int javaSurface(JNIEnv*env,jobject surface,KLJMediaSurface*out){void*window=klj_surface_native_window(env,surface);if(!window)return invalid;int result=acquire(window,out);klj_surface_release_window(window);return result;}
}
extern "C" void klj_video_initialize(JNIEnv*env){
    std::call_once(initialized,[&]{env->GetJavaVM(&vm);auto type=env->FindClass("maclauncher/android/framework/HostNativeHandle");if(!type)return;handles=(jclass)env->NewGlobalRef(type);env->DeleteLocalRef(type);retainHandle=env->GetStaticMethodID(handles,"retain","(J)J");releaseHandle=env->GetStaticMethodID(handles,"release","(J)V");if(env->ExceptionCheck())return;
        type=env->FindClass("maclauncher/android/framework/HostSurface");if(!type)return;surfaces=(jclass)env->NewGlobalRef(type);env->DeleteLocalRef(type);targetKind=env->GetStaticMethodID(surfaces,"videoTarget","(J)I");submitPixels=env->GetStaticMethodID(surfaces,"submitVideoPixels","(J[IIIJ)Z");if(!env->ExceptionCheck())klj_media_surface_install(javaSurface,acquire);});
}
