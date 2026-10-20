// Owned Android/ANGLE presentation bridge. Upstream Klepton remains unchanged.
// ANGLE's IOSurface and Metal shared-event extensions provide the same surface
// boundary used by Klepton's flat viewer; this owns the actual app EGL surface.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include "presentation.h"
#include "angle-metal.hpp"
#include "surface-format.hpp"
#include "framebuffer-initialize.hpp"
#include "overlay.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <mutex>
#include <atomic>
#include <vector>
#include <array>
#include <algorithm>
#include "frame.h"
#include "shared-presentation.h"
#include "target-time.h"
#include "surface-target.hpp"
#include "pixel-copy.hpp"
#include "frame-observation.hpp"
extern "C" void *klj_graphics_symbol(const char *name);

namespace {
struct Presenter {
    std::mutex frameMutex;
    std::atomic<uint64_t> committedExtent{0};
    klj_target::Surface target;
    klj_copy::Queue copies;
    CAMetalLayer *layer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLSharedEvent> produced,consumed;
    id<MTLTexture> source;
    id<MTLRenderPipelineState> displayPipeline;
    id<MTLCommandBuffer> last;
    uint64_t serial=0,uploadedUi=0;
    unsigned width,height,outputWidth,outputHeight,rendered=0,accepted=0,busy=0;
    klj_timing::Reporter glesObservation,nativeObservation,canvasObservation,overlayObservation,projectionObservation;
    uint64_t presentationGeneration=0;
    std::string lastRejection;
    bool outputAssigned=false,sharedTopLeft=false;
    bool initialized=false,deviceReady=false;
    bool projection=false;
    IOSurfaceRef storage=nullptr;
    std::array<IOSurfaceID,3> producerBuffers{};
    void *display=nullptr,*surface=nullptr,*producerSync=nullptr,*consumerSync=nullptr;
    void *effectsLibrary=nullptr;
    void *shared=nullptr;
    void *overlay=nullptr;
    void *(*createShared)(void*,void*,const RSPresenterOptions*,RSPresenterError*)=nullptr;
    int (*submitShared)(void*,const RSPresenterSubmission*,RSFrameResult*)=nullptr;
    int (*sharedStatus)(void*,RSPresenterStatus*)=nullptr;
    int (*submitTarget)(void*,const RSPresenterSubmission*,const RSPresentationTarget*,RSFrameResult*)=nullptr;
    int (*targetTiming)(void*,RSPresentationTiming*)=nullptr;
    void (*destroyShared)(void*)=nullptr;
    void (*releaseOwner)(void*)=nullptr;
    void *(*retainNativeSource)(void*,void*)=nullptr;
    void (*releaseNativeSource)(void*)=nullptr;
    ~Presenter(){if(overlay)klj_overlay_destroy(overlay);if(shared)destroyShared(shared);if(releaseOwner)releaseOwner(this);if(storage)CFRelease(storage);/* Runtime may own callbacks: keep dylib loaded to process exit. */}
};
struct FrameMeasurement {
    Presenter *p;
    klj_timing::Reporter &observation;
    const char *path;
    uint64_t started=uint64_t(CACurrentMediaTime()*1e9);
    ~FrameMeasurement() noexcept {
        const auto now=uint64_t(CACurrentMediaTime()*1e9);
        observation.record(started,now,path,"application",{p->shared,p->sharedStatus,p->presentationGeneration,
            p->accepted,p->busy,p->width,p->height,p->outputWidth,p->outputHeight});
    }
};
thread_local Presenter *active=nullptr;
thread_local IOSurfaceRef suppliedStorage=nullptr;
void fail(JNIEnv *env,const char *message){if(env->ExceptionCheck())return;jclass type=env->FindClass("java/lang/IllegalStateException");if(type)env->ThrowNew(type,message);}
template<class T>T symbol(const char *name){auto p=klj_graphics_symbol(name);if(!p)throw std::runtime_error(std::string("Missing ANGLE presentation function: ")+name);return reinterpret_cast<T>(p);}
void check(bool result,const char *what){if(!result)throw std::runtime_error(what);}
int submitFrame(Presenter *p,const RSPresenterSubmission *submission,const RSPresentationTarget *target,RSFrameResult *result){
    return target?(p->submitTarget?p->submitTarget(p->shared,submission,target,result):RS_FRAME_INVALID_ARGUMENT):p->submitShared(p->shared,submission,result);
}
void *sharedLibrary(const char *path){
    // Exactly one process-lifetime reference; callbacks may outlive a presenter.
    struct Library {std::mutex mutex;std::string path;void *handle=nullptr;};
    static auto *library=new Library;std::lock_guard lock(library->mutex);
    if(library->handle){check(library->path==path,"Shared graphics library cannot change in this process");return library->handle;}
    void *handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);check(handle,dlerror()?:"Cannot load shared graphics presenter");
    for(auto name:{"rsCreateFramePresenter","rsSubmitFramePresenter","rsGetFramePresenterStatus","rsDestroyFramePresenter"})if(!dlsym(handle,name)){dlclose(handle);throw std::runtime_error("Shared graphics library lacks the presentation API");}
    library->path=path;library->handle=handle;return handle;
}
const char *displayShader=R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Vertex {float4 position [[position]];float2 uv;};
vertex Vertex hostVertex(uint id [[vertex_id]]) {
    float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    Vertex out;out.position=float4(p[id],0,1);out.uv=(p[id]+1)*0.5;return out;
}
fragment float4 hostFragment(Vertex in [[stage_in]],texture2d<float> source [[texture(0)]]) {
    constexpr sampler sample(coord::normalized,address::clamp_to_edge,filter::nearest);
    return source.sample(sample,in.uv);
}
)METAL";
// The SDL-window presenter outlives individual EGL surfaces. finish() retires
// the shared presenter, while its validated library/device/queue remain owned.
// Recreate only that handle when another EGL surface starts on the same owner.
void initializeShared(Presenter *p,bool topLeft=false){
    topLeft=topLeft||p->projection;
    if(p->shared&&p->sharedTopLeft!=topLeft){p->destroyShared(p->shared);p->shared=nullptr;}
    if(p->shared||!p->createShared)return;
    RSPresenterOptions options={};options.size=sizeof(options);options.version=RS_PRESENTER_ABI_VERSION;
    options.origin=topLeft?RS_FRAME_TOP_LEFT:RS_FRAME_BOTTOM_LEFT;options.flags=topLeft?0:RS_PRESENTER_FLIP_Y;
    RSPresenterError error={};error.size=sizeof(error);error.version=RS_PRESENTER_ABI_VERSION;
    p->shared=p->createShared((__bridge void*)p->layer,p,&options,&error);check(p->shared,error.reason);p->sharedTopLeft=topLeft;++p->presentationGeneration;
}
void initializeDevice(Presenter *p,id<MTLDevice> device,bool topLeft=false){
    p->device=device;
    p->queue=[p->device newCommandQueue];p->produced=[p->device newSharedEvent];p->consumed=[p->device newSharedEvent];
    check(p->queue&&p->produced&&p->consumed,"Metal queue/shared events are unavailable");
    p->layer.device=p->device;p->layer.pixelFormat=MTLPixelFormatBGRA8Unorm;p->layer.framebufferOnly=NO;
    p->layer.drawableSize=CGSizeMake(p->outputWidth,p->outputHeight);p->layer.maximumDrawableCount=3;p->layer.displaySyncEnabled=YES;
    const char *effects=getenv("KLEPTON_FRAME_LIBRARY");
    check(!p->overlay||(effects&&*effects),"Native overlay requires the common graphics presenter");
    if(effects&&*effects){
        p->effectsLibrary=sharedLibrary(effects);
        p->createShared=reinterpret_cast<decltype(p->createShared)>(dlsym(p->effectsLibrary,"rsCreateFramePresenter"));
        p->submitShared=reinterpret_cast<decltype(p->submitShared)>(dlsym(p->effectsLibrary,"rsSubmitFramePresenter"));
        p->sharedStatus=reinterpret_cast<decltype(p->sharedStatus)>(dlsym(p->effectsLibrary,"rsGetFramePresenterStatus"));
        p->submitTarget=reinterpret_cast<decltype(p->submitTarget)>(dlsym(p->effectsLibrary,"rsSubmitFramePresenterAtTime"));
        p->targetTiming=reinterpret_cast<decltype(p->targetTiming)>(dlsym(p->effectsLibrary,"rsGetFramePresenterTiming"));
        p->destroyShared=reinterpret_cast<decltype(p->destroyShared)>(dlsym(p->effectsLibrary,"rsDestroyFramePresenter"));
        p->releaseOwner=reinterpret_cast<decltype(p->releaseOwner)>(dlsym(p->effectsLibrary,"rsReleaseFrameOwner"));
        p->retainNativeSource=reinterpret_cast<decltype(p->retainNativeSource)>(dlsym(p->effectsLibrary,"rsRetainNativeMetalSource"));
        p->releaseNativeSource=reinterpret_cast<decltype(p->releaseNativeSource)>(dlsym(p->effectsLibrary,"rsReleaseNativeMetalSource"));
        check(p->createShared&&p->submitShared&&p->sharedStatus&&p->destroyShared,"Shared graphics library lacks the presentation API");
        initializeShared(p,topLeft);
    }
    if(!p->shared){
    NSError *error=nil;auto library=[p->device newLibraryWithSource:@(displayShader) options:nil error:&error];
    check(library!=nil,error.localizedDescription.UTF8String?:"Presentation shader compilation failed");
    auto descriptor=[MTLRenderPipelineDescriptor new];descriptor.vertexFunction=[library newFunctionWithName:@"hostVertex"];descriptor.fragmentFunction=[library newFunctionWithName:@"hostFragment"];descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    p->displayPipeline=[p->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    check(p->displayPipeline!=nil,error.localizedDescription.UTF8String?:"Presentation pipeline creation failed");
    }
    p->deviceReady=true;
    fprintf(stderr,"[android-metal] device=%s shared presenter=%s\n",p->device.name.UTF8String,p->shared?"enabled":"off");
}
void initialize(Presenter *p,void *display){
    const char *extensions=symbol<const char*(*)(void*,int)>("eglQueryString")(display,0x3055);
    check(extensions&&strstr(extensions,"EGL_ANGLE_iosurface_client_buffer")&&strstr(extensions,"EGL_ANGLE_metal_shared_event_sync"),"ANGLE does not support IOSurface and shared-event presentation");
    initializeDevice(p,klj_angle::device(display));
}
void *sync(Presenter *p,id<MTLSharedEvent> event,uint64_t value,bool external){
    void *result=klj_angle::sharedSync(p->display,event,value,external);
    check(result,"ANGLE could not create a Metal shared-event fence");return result;
}
void removeSync(Presenter *p,void *&value){if(value){check(symbol<unsigned(*)(void*,void*)>("eglDestroySync")(p->display,value),"ANGLE could not retire a presentation fence");value=nullptr;}}

}
extern "C" void *klj_metal_create(JNIEnv *env,void *layer,int width,int height){@autoreleasepool{
    if(!layer||width<1||height<1||width>8192||height>8192){fail(env,"Invalid Metal host layer or dimensions");return nullptr;}
    auto p=new Presenter;p->layer=(__bridge CAMetalLayer*)layer;p->width=width;p->height=height;
    p->outputWidth=width;p->outputHeight=height;return p;
}}
extern "C" int klj_metal_output_size(JNIEnv *env,void *value,int width,int height){@autoreleasepool{try{
    auto p=static_cast<Presenter*>(value);check(p&&width>0&&height>0&&width<=8192&&height<=8192,"Invalid Android output extent");
    std::lock_guard lock(p->frameMutex);p->outputWidth=width;p->outputHeight=height;p->outputAssigned=true;
    p->layer.drawableSize=CGSizeMake(width,height);return 1;
}catch(const std::exception &error){fail(env,error.what());return 0;}}}
extern "C" void klj_metal_enter(void *presenter){active=(Presenter*)presenter;}
extern "C" int klj_metal_resize(JNIEnv *env,int width,int height){@autoreleasepool{try{
    auto p=active;check(p&&p->surface&&width>=1&&height>=1&&width<=8192&&height<=8192,"Invalid Android surface resize");
    std::lock_guard lock(p->frameMutex);
    // Resizing retires the previous image before EGL releases its pbuffer.
    // Ordinary frames remain entirely GPU synchronized.
    symbol<void(*)(void)>("glFlush")();[p->last waitUntilCompleted];
    check(!p->last||p->last.status==MTLCommandBufferStatusCompleted,"Metal surface could not finish before resize");
    removeSync(p,p->producerSync);removeSync(p,p->consumerSync);
    p->copies.retire();p->last=nil;p->source=nil;p->producerBuffers={};
    if(p->storage){CFRelease(p->storage);p->storage=nullptr;}
    p->target.retire();p->surface=nullptr;p->initialized=false;p->width=width;p->height=height;
    if(!p->outputAssigned){p->outputWidth=width;p->outputHeight=height;}
    p->layer.drawableSize=CGSizeMake(p->outputWidth,p->outputHeight);
    return 1;
}catch(const std::exception &error){fail(env,error.what());return 0;}}}
extern "C" int klj_metal_current(JNIEnv *env,void *surface){@autoreleasepool{try{
    if(!active||active->surface!=surface||active->initialized)return 1;
    klj_framebuffer::initialize(klj_graphics_symbol);
    active->initialized=true;return 1;
}catch(const std::exception &error){fail(env,error.what());return 0;}}}
extern "C" void *klj_metal_surface(JNIEnv *env,void *display,void *config,int width,int height,bool opaque,const int *extraAttributes){@autoreleasepool{try{
    if(!active)return nullptr;auto p=active;
    std::lock_guard lock(p->frameMutex);
    check(!p->surface&&width>0&&height>0&&width<=8192&&height<=8192,"Metal host requires one valid Android window surface");
    // AOSP SurfaceView/GLSurfaceView may recreate its EGL producer after the
    // native window resizes. Its new buffer owns the render extent.
    p->width=width;p->height=height;
    if(!p->deviceReady)initialize(p,display);else{check(p->device.registryID==klj_angle::device(display).registryID,"Android producer uses a different Metal device");initializeShared(p);}p->display=display;
    NSDictionary *properties=@{(__bridge NSString*)kIOSurfaceWidth:@(width),(__bridge NSString*)kIOSurfaceHeight:@(height),(__bridge NSString*)kIOSurfaceBytesPerElement:@4,(__bridge NSString*)kIOSurfaceBytesPerRow:@(width*4),(__bridge NSString*)kIOSurfacePixelFormat:@((uint32_t)'BGRA')};
    if(suppliedStorage){p->storage=suppliedStorage;suppliedStorage=nullptr;check(IOSurfaceGetWidth(p->storage)==(size_t)width&&IOSurfaceGetHeight(p->storage)==(size_t)height,"Native buffer geometry mismatch");}else p->storage=IOSurfaceCreate((__bridge CFDictionaryRef)properties);check(p->storage,"IOSurface allocation failed");
    int target=0;check(symbol<unsigned(*)(void*,void*,int,int*)>("eglGetConfigAttrib")(display,config,0x348D,&target),"ANGLE surface texture target is unavailable");
    std::vector<int> attributes={0x3057,width,0x3056,height,0x345A,0,0x3081,target,0x3080,0x305E,0x345C,klj_surface_format::glType(p->storage),0x345D,klj_surface_format::glFormat(p->storage,opaque)};
    if(extraAttributes)for(size_t i=0;extraAttributes[i]!=0x3038;i+=2){attributes.push_back(extraAttributes[i]);attributes.push_back(extraAttributes[i+1]);}attributes.push_back(0x3038);
    p->surface=symbol<void*(*)(void*,unsigned,void*,void*,const int*)>("eglCreatePbufferFromClientBuffer")(display,0x3454,p->storage,config,attributes.data());check(p->surface,"ANGLE IOSurface window creation failed");
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:klj_surface_format::metal(p->storage) width:width height:height mipmapped:NO];descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
    p->source=[p->device newTextureWithDescriptor:descriptor iosurface:p->storage plane:0];check(p->source!=nil,"Cannot wrap Android IOSurface as a Metal texture");
    p->producerBuffers[0]=IOSurfaceGetID(p->storage);
    fprintf(stderr,"[android-metal] source=%ux%u output=%ux%u\n",unsigned(p->source.width),unsigned(p->source.height),p->outputWidth,p->outputHeight);
    p->committedExtent.store((uint64_t(uint32_t(width))<<32)|uint32_t(height),std::memory_order_release);
    p->target.bind(display,p->surface);
    return p->surface;
}catch(const std::exception &error){fail(env,error.what());return nullptr;}}}
static int present(JNIEnv *env,void *presenter,int frame,std::shared_ptr<void> sourceLease,const KLMetalBufferSwap *swap){@autoreleasepool{try{
    auto p=(Presenter*)presenter;check(p&&p->surface,"Android has no IOSurface-backed EGL surface");
    std::lock_guard lock(p->frameMutex);
    // XR already presents the composed frame through this owner. The ordinary
    // surface is no longer sampled, so do not put a second wait on its queue.
    if(p->projection){symbol<void(*)(void)>("glFlush")();return 2;}
    auto submittedImage=p->source;id<MTLTexture> nextImage=nil;IOSurfaceID nextId=0;
    if(swap){
        check(sourceLease&&swap->surface&&swap->storage&&swap->advance,"EGL producer handoff is incomplete");
        auto storage=static_cast<IOSurfaceRef>(swap->storage);nextId=IOSurfaceGetID(storage);
        check(IOSurfaceGetWidth(storage)==p->width&&IOSurfaceGetHeight(storage)==p->height,"EGL producer image extent changed");
        nextImage=klj_surface_format::texture(p->device,storage);check(nextImage,"Cannot import the next EGL producer image");
        check(std::find(p->producerBuffers.begin(),p->producerBuffers.end(),nextId)!=p->producerBuffers.end()||std::find(p->producerBuffers.begin(),p->producerBuffers.end(),0)!=p->producerBuffers.end(),"EGL producer image pool exceeds three buffers");
    }
    FrameMeasurement measurement{p,p->glesObservation,"gles"};
    if(frame==12){
        int framebuffer=-1;symbol<void(*)(unsigned,int*)>("glGetIntegerv")(0x8CA6,&framebuffer);int readbuffer=framebuffer;
        const char *version=(const char*)symbol<const unsigned char*(*)(unsigned)>("glGetString")(0x1F02);
        if(klj_framebuffer::separateRead(version))symbol<void(*)(unsigned,int*)>("glGetIntegerv")(0x8CAA,&readbuffer);
        auto currentSurface=symbol<void*(*)(int)>("eglGetCurrentSurface")(0x3059);auto currentDisplay=symbol<void*(*)()>("eglGetCurrentDisplay")();auto currentContext=symbol<void*(*)()>("eglGetCurrentContext")();
        fprintf(stderr,"[android-metal] actual EGL display=%p expected=%p surface=%p expected=%p context=%p drawFBO=%d readFBO=%d\n",currentDisplay,p->display,currentSurface,p->surface,currentContext,framebuffer,readbuffer);
    }
    check(!p->last||p->last.status!=MTLCommandBufferStatusError,p->last.error.localizedDescription.UTF8String?:"Prior Metal presentation failed");
    removeSync(p,p->producerSync);removeSync(p,p->consumerSync);
    uint64_t serial=++p->serial;p->producerSync=sync(p,p->produced,serial,false);
    symbol<void(*)(void)>("glFlush")();
    auto commands=[p->queue commandBuffer];commands.label=@"Android common graphics and presentation";
    [commands encodeWaitForEvent:p->produced value:serial];
    p->copies.encode(commands,p->source,true);
    if(swap&&swap->preserved){
        auto next=nextImage;
        check(next&&next.width==p->source.width&&next.height==p->source.height&&next.pixelFormat==p->source.pixelFormat,"Preserved EGL buffer extent changed");
        auto copy=[commands blitCommandEncoder];check(copy,"Cannot preserve the EGL window buffer");
        [copy copyFromTexture:p->source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(next.width,next.height,1)
            toTexture:next destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[copy endEncoding];
    }
    std::string submissionError;
    int64_t targetNanos=0;const bool targeted=p->target.read(targetNanos);bool targetRefused=false;
    void *frameSource=(__bridge void*)p->source;int overlayStatus=0;KLJOverlayFrame overlayFrame{};
    if(p->projection)overlayStatus=3;
    else if(p->overlay){
        overlayStatus=klj_overlay_set_surface(env,p->overlay,(__bridge void*)p->device,p->width,p->height);
        if(overlayStatus==0){overlayStatus=klj_overlay_encode(env,p->overlay,(__bridge void*)commands,frameSource,p->sharedTopLeft?1:0,&overlayFrame);frameSource=overlayFrame.texture;}
        if(overlayStatus==3){p->busy++;if(targeted)targetRefused=true;}
        else if(overlayStatus!=0)submissionError="Native overlay composition failed";
    }
    if(overlayStatus==0&&p->shared){
        RSPresenterSubmission submission={};submission.size=sizeof(submission);submission.version=RS_PRESENTER_ABI_VERSION;
        submission.commandBuffer=(__bridge void*)commands;submission.sourceTexture=frameSource;
        submission.frameId=serial;submission.sourceTime=CACurrentMediaTime();
        RSFrameResult result={};result.size=sizeof(result);result.version=RS_FRAME_ABI_VERSION;
        const RSPresentationTarget target{sizeof(target),RS_TARGET_ABI_VERSION,static_cast<double>(targetNanos)/1e9};
        int status=submitFrame(p,&submission,targeted?&target:nullptr,&result);
        if(targeted&&status!=RS_FRAME_OK){
            std::string reason=*result.reason?result.reason:(p->submitTarget?"Shared presenter rejected the requested frame":"Shared presenter has no presentation-time entrypoint");
            if(reason!=p->lastRejection){fprintf(stderr,"[android-metal] presentation rejected: %s; status=%d target=%lld host=%lld source=%ux%u output=%ux%u\n",reason.c_str(),status,(long long)targetNanos,(long long)(CACurrentMediaTime()*1e9),result.inputWidth,result.inputHeight,result.outputWidth,result.outputHeight);p->lastRejection=std::move(reason);}
        }else if(status==RS_FRAME_OK)p->lastRejection.clear();
        if(status==RS_FRAME_BUSY){p->busy++;if(targeted)targetRefused=true;}
        else if(targeted&&status!=RS_FRAME_OK)targetRefused=true;
        else if(status==RS_FRAME_OK&&result.encoded){p->accepted++;if(p->overlay)klj_overlay_frame_submitted(p->overlay,&overlayFrame);}
        else submissionError=*result.reason?result.reason:"Shared graphics submission failed";
    }else if(overlayStatus==0){
    auto drawable=[p->layer nextDrawable];check(drawable!=nil,"Metal drawable unavailable");
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    auto encoder=[commands renderCommandEncoderWithDescriptor:pass];[encoder setRenderPipelineState:p->displayPipeline];[encoder setFragmentTexture:p->source atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    [commands presentDrawable:drawable];p->accepted++;if(p->overlay)klj_overlay_frame_submitted(p->overlay,&overlayFrame);
    }
    // The next ANGLE render must not overwrite the shared surface before our
    // sampling finishes. A GPU-side wait, not glFinish or a CPU event wait.
    [commands encodeSignalEvent:p->consumed value:serial];
    p->consumerSync=sync(p,p->consumed,serial,true);
    if(swap){
        // Rebind before publishing the immutable old source. Failure leaves
        // the old producer current, so its image must remain unpublished.
        check(swap->advance(swap->context),"ANGLE cannot advance the EGL producer buffer");
        auto previous=p->storage;p->storage=(IOSurfaceRef)CFRetain(swap->storage);if(previous)CFRelease(previous);
        p->source=nextImage;p->surface=swap->surface;p->target.rebind(p->display,swap->surface);
        if(std::find(p->producerBuffers.begin(),p->producerBuffers.end(),nextId)==p->producerBuffers.end())*std::find(p->producerBuffers.begin(),p->producerBuffers.end(),0)=nextId;
    }
    check(symbol<unsigned(*)(void*,void*,int)>("eglWaitSync")(p->display,p->consumerSync,0),"ANGLE cannot wait for shared-surface reuse");
    // Future producer writes now target another owned slot. Independent idle
    // readbacks retain this actual submitted image through GPU completion.
    if(sourceLease)p->copies.source(commands,submittedImage,true,std::move(sourceLease));else p->copies.clearSource();
    [commands commit];p->last=commands;
    // Rejected targeted swaps still retire producer/consumer fences, but never
    // claim success for an ignored target. The EGL adapter reports BAD_ACCESS.
    if(targetRefused)return 0;
    check(submissionError.empty(),submissionError.c_str());
    p->rendered++;return 1;
}catch(const std::exception &error){fail(env,error.what());return 0;}}}
extern "C" int klj_metal_present(JNIEnv *env,void *presenter,int frame){return present(env,presenter,frame,{},nullptr);}
extern "C" int klj_metal_present_retained(JNIEnv *env,void *presenter,int frame,void *owner,void(*release)(void*),const KLMetalBufferSwap *swap){return present(env,presenter,frame,std::shared_ptr<void>(owner,release),swap);}
extern "C" int klj_metal_replace_surface(JNIEnv *env,void *presenter,void *surface,void *storage,int width,int height){@autoreleasepool{try{
    auto p=static_cast<Presenter*>(presenter);check(p&&surface&&storage&&width>0&&height>0&&width<=8192&&height<=8192,"Invalid EGL backing generation");
    std::lock_guard lock(p->frameMutex);auto image=static_cast<IOSurfaceRef>(storage);
    check(IOSurfaceGetWidth(image)==size_t(width)&&IOSurfaceGetHeight(image)==size_t(height),"EGL backing generation extent differs");
    auto texture=klj_surface_format::texture(p->device,image);check(texture,"Cannot import the replacement EGL backing");
    // Preparation and the actual EGL bind have succeeded. No owner metadata
    // changes until the old producer's submitted GPU work is known complete.
    if(symbol<void*(*)()>("eglGetCurrentContext")()&&symbol<void*(*)()>("eglGetCurrentDisplay")()==p->display)symbol<void(*)()>("glFlush")();
    [p->last waitUntilCompleted];check(!p->last||p->last.status==MTLCommandBufferStatusCompleted,"Metal surface could not finish before resize");
    removeSync(p,p->producerSync);removeSync(p,p->consumerSync);
    auto previous=p->storage;p->storage=(IOSurfaceRef)CFRetain(image);if(previous)CFRelease(previous);
    p->copies.retire();p->last=nil;p->source=texture;p->surface=surface;p->initialized=false;
    p->producerBuffers={IOSurfaceGetID(image),0,0};p->width=width;p->height=height;
    if(!p->outputAssigned){p->outputWidth=width;p->outputHeight=height;}
    p->layer.drawableSize=CGSizeMake(p->outputWidth,p->outputHeight);p->target.bind(p->display,surface);
    p->committedExtent.store((uint64_t(uint32_t(width))<<32)|uint32_t(height),std::memory_order_release);return 1;
}catch(const std::exception &error){fail(env,error.what());return 0;}}}
extern "C" void *klj_metal_projection_queue(JNIEnv *env,void *presenter,void *device){@autoreleasepool{try{
    auto p=static_cast<Presenter*>(presenter);check(p&&device,"Projection graphics device is unavailable");
    std::lock_guard lock(p->frameMutex);auto actual=(__bridge id<MTLDevice>)device;
    if(!p->deviceReady)initializeDevice(p,actual);
    check(p->device==actual&&p->createShared,"Projection requires the same device and common graphics presenter");
    if(!p->projection){if(p->shared){p->destroyShared(p->shared);p->shared=nullptr;}p->projection=true;initializeShared(p);}
    return (__bridge void*)p->queue;
}catch(const std::exception &error){fail(env,error.what());return nullptr;}}}
static int project(JNIEnv *env,void *presenter,void *display,void *buffer,void *texture,bool nativeProducer,std::shared_ptr<void> sourceLease={}){@autoreleasepool{try{
    auto p=static_cast<Presenter*>(presenter);check(p&&buffer&&texture,"Android projection source is unavailable");
    std::unique_lock lock(p->frameMutex,std::try_to_lock);if(!lock.owns_lock())return 3;
    FrameMeasurement measurement{p,nativeProducer?p->nativeObservation:p->projectionObservation,nativeProducer?"native":"projection"};
    auto commands=(__bridge id<MTLCommandBuffer>)buffer;auto source=(__bridge id<MTLTexture>)texture;
    check(commands.status==MTLCommandBufferStatusNotEnqueued&&source.device==commands.device,"Projection command buffer ownership is invalid");
    if(!p->deviceReady){if(display)initialize(p,display);else initializeDevice(p,source.device);}
    check(p->device==source.device,"Projection source uses a different graphics device");
    if(nativeProducer){p->width=source.width;p->height=source.height;}
    check(source.width==p->width&&source.height==p->height,"Projection source does not match the Android surface extent");
    check(p->createShared&&p->submitShared,"Projection requires the common graphics presenter");
    if(!p->projection){if(p->shared){p->destroyShared(p->shared);p->shared=nullptr;}p->projection=true;}
    initializeShared(p);
    p->source=source;
    p->copies.encode(commands,source,false);
    void *frameSource=texture;KLJOverlayFrame overlayFrame{};
    if(p->overlay){int status=klj_overlay_set_surface(env,p->overlay,(__bridge void*)p->device,p->width,p->height);if(!status)status=klj_overlay_encode(env,p->overlay,buffer,texture,p->sharedTopLeft?1:0,&overlayFrame);if(status)return status;frameSource=overlayFrame.texture;}
    RSPresenterSubmission submission{};submission.size=sizeof(submission);submission.version=RS_PRESENTER_ABI_VERSION;
    submission.commandBuffer=buffer;submission.sourceTexture=frameSource;submission.frameId=++p->serial;submission.sourceTime=CACurrentMediaTime();
    RSFrameResult result{};result.size=sizeof(result);result.version=RS_FRAME_ABI_VERSION;
    int64_t nanos=0;bool targeted=p->target.read(nanos);
    RSPresentationTarget target{sizeof(target),RS_TARGET_ABI_VERSION,double(nanos)/1e9};
    int status=submitFrame(p,&submission,targeted?&target:nullptr,&result);
    if(status==RS_FRAME_BUSY){++p->busy;return 3;}
    check(status==RS_FRAME_OK&&result.encoded,*result.reason?result.reason:"Shared projection submission failed");
    if(nativeProducer&&!sourceLease&&p->retainNativeSource&&p->releaseNativeSource){
        if(auto owner=p->retainNativeSource(buffer,texture))sourceLease=std::shared_ptr<void>(owner,p->releaseNativeSource);
    }
    if(sourceLease)p->copies.source(commands,source,false,std::move(sourceLease));
    else p->copies.snapshot(commands,source,false);
    if(p->overlay)klj_overlay_frame_submitted(p->overlay,&overlayFrame);
    ++p->accepted;++p->rendered;
    p->committedExtent.store((uint64_t(p->width)<<32)|p->height,std::memory_order_release);
    return 0;
}catch(const std::exception &error){fail(env,error.what());return 4;}}}
extern "C" int klj_metal_project(JNIEnv *env,void *presenter,void *display,void *buffer,void *texture){return project(env,presenter,display,buffer,texture,false);}
extern "C" int klj_metal_project_retained(JNIEnv *env,void *presenter,void *display,void *buffer,void *texture,void *owner,void(*release)(void*)){
    return project(env,presenter,display,buffer,texture,false,std::shared_ptr<void>(owner,release));
}
extern "C" int klj_metal_native_frame(JNIEnv *env,void *presenter,void *buffer,void *texture){return project(env,presenter,nullptr,buffer,texture,true);}
namespace {char nativePresenterKey;}
extern "C" void *klj_metal_native_layer(JNIEnv *env,int width,int height,KLNativeMetalFrame frame,void *context){@autoreleasepool{try{
    check(width>0&&height>0,"Native Vulkan surface is unavailable");
    const char *library=getenv("KLEPTON_FRAME_LIBRARY");check(library&&*library,"Vulkan requires the common graphics presenter");
    auto handle=sharedLibrary(library);
    auto add=reinterpret_cast<int(*)(void*,KLNativeMetalFrame,void*)>(dlsym(handle,"rsRegisterNativeMetalLayer"));
    auto remove=reinterpret_cast<void(*)(void*)>(dlsym(handle,"rsUnregisterNativeMetalLayer"));
    check(add&&remove,"Graphics presenter lacks the native Vulkan layer API");
    auto layer=[CAMetalLayer layer];layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=NO;
    layer.drawableSize=CGSizeMake(width,height);layer.frame=CGRectMake(0,0,width,height);layer.contentsScale=1;
    check(add((__bridge void*)layer,frame,context),"Native Vulkan layer registration failed");
    objc_setAssociatedObject(layer,&nativePresenterKey,[NSValue valueWithPointer:reinterpret_cast<void*>(remove)],OBJC_ASSOCIATION_RETAIN);
    return (__bridge_retained void*)layer;
}catch(const std::exception &error){fail(env,error.what());return nullptr;}}}
extern "C" void klj_metal_release_native_layer(void *value){@autoreleasepool{
    if(!value)return;auto layer=(__bridge_transfer CAMetalLayer*)value;
    auto remove=reinterpret_cast<void(*)(void*)>([(NSValue*)objc_getAssociatedObject(layer,&nativePresenterKey) pointerValue]);
    if(remove)remove((__bridge void*)layer);
}}
extern "C" int klj_metal_finish(JNIEnv *env,void *presenter){@autoreleasepool{try{
    auto p=(Presenter*)presenter;if(!p)return 0;
    std::lock_guard lock(p->frameMutex);
    // Unity requests its last surface image inside surfaceDestroyed, before
    // its producer destroys EGL. Service that request while the actual context
    // still owns the source; an arbitrary UI-thread flush is not a fence.
    if(p->copies.waiting()){
        bool current=p->surface&&!p->projection&&p->rendered&&
            symbol<void*(*)()>("eglGetCurrentContext")()!=nullptr&&
            symbol<void*(*)()>("eglGetCurrentDisplay")()==p->display&&
            symbol<void*(*)(int)>("eglGetCurrentSurface")(0x3059)==p->surface;
        if(current){
            removeSync(p,p->producerSync);removeSync(p,p->consumerSync);
            uint64_t serial=++p->serial;p->producerSync=sync(p,p->produced,serial,false);
            symbol<void(*)(void)>("glFlush")();auto commands=[p->queue commandBuffer];
            [commands encodeWaitForEvent:p->produced value:serial];
            p->copies.encode(commands,p->source,true);
            [commands encodeSignalEvent:p->consumed value:serial];[commands commit];p->last=commands;
            p->consumerSync=sync(p,p->consumed,serial,true);
            check(symbol<unsigned(*)(void*,void*,int)>("eglWaitSync")(p->display,p->consumerSync,0),"ANGLE cannot retire the surface copy");
        }else p->copies.retire(p->rendered?klj_copy::SourceInvalid:klj_copy::NoData);
    }
    // A single teardown drain; no per-frame CPU completion waits or readback.
    if(p->surface){symbol<void(*)(void)>("glFlush")();[p->last waitUntilCompleted];check(!p->last||p->last.status==MTLCommandBufferStatusCompleted,p->last.error.localizedDescription.UTF8String?:"Metal presentation failed during teardown");removeSync(p,p->producerSync);removeSync(p,p->consumerSync);}
    if(p->shared){RSPresenterStatus status={};status.size=sizeof(status);status.version=RS_PRESENTER_ABI_VERSION;
        check(p->sharedStatus(p->shared,&status)==RS_FRAME_OK,"Cannot inspect shared graphics completion");
        fprintf(stderr,"[android-metal] shared encoded=%llu real=%llu generated=%llu busy=%u pending=%u\n",(unsigned long long)status.encoded,(unsigned long long)status.presentedReal,(unsigned long long)status.presentedGenerated,p->busy,status.pending);
        p->destroyShared(p->shared);p->shared=nullptr;
    }
    p->copies.clearSource();
    fprintf(stderr,"[android-metal] frames=%u producer=%llu consumer=%llu per-frame-readbacks=0 explicit-CPU-completion-waits=0\n",p->rendered,(unsigned long long)p->produced.signaledValue,(unsigned long long)p->consumed.signaledValue);
    return p->rendered>0&&p->accepted>0;
}catch(const std::exception &error){fail(env,error.what());return 0;}}}
extern "C" void klj_metal_destroy(void *presenter){@autoreleasepool{delete (Presenter*)presenter;}}

extern "C" uint64_t klj_metal_copy(JNIEnv *env,void *presenter,void *storage,int width,int height,jobject callback){return klj_metal_copy_region(env,presenter,storage,0,0,0,0,width,height,callback);}
extern "C" uint64_t klj_metal_copy_region(JNIEnv *env,void *presenter,void *storage,int left,int top,int right,int bottom,int width,int height,jobject callback){@autoreleasepool{
    auto p=static_cast<Presenter*>(presenter);
    if(!p){klj_copy::Queue unavailable;return unavailable.add(env,callback,width,height,klj_copy::NoData);}
    std::lock_guard lock(p->frameMutex);
    int status=-1;
    if(width<=0||height<=0||width>16384||height>16384||uint64_t(width)*height>16777216)status=klj_copy::DestinationInvalid;
    else if(!p->projection&&p->storage&&(!storage||std::find(p->producerBuffers.begin(),p->producerBuffers.end(),IOSurfaceGetID(static_cast<IOSurfaceRef>(storage)))==p->producerBuffers.end()))status=klj_copy::SourceInvalid;
    else if(!p->source||!p->rendered)status=klj_copy::NoData;
    return p->copies.add(env,callback,width,height,status,left,top,right,bottom);
}}

extern "C" int klj_metal_overlay_attach(JNIEnv*env,void*presenter,int width,int height){@autoreleasepool{try{
    auto p=(Presenter*)presenter;check(p&&!p->device&&!p->surface&&!p->overlay,"Attach overlay before the native producer starts");
    check(width==(int)p->width&&height==(int)p->height,"Overlay initial extent differs from its native window");
    p->overlay=klj_overlay_create(env,width,height);return p->overlay?0:4;
}catch(const std::exception&e){fail(env,e.what());return 4;}}}
extern "C" int klj_metal_overlay_validate_size(JNIEnv*env,void*presenter,int width,int height){auto p=(Presenter*)presenter;if(!p||!p->overlay){fail(env,"Native overlay is not attached");return 4;}return klj_overlay_validate_size(env,p->overlay,width,height);}
extern "C" int klj_metal_overlay_submit(JNIEnv*env,void*presenter,jintArray pixels,int offset,int stride,int width,int height,uint64_t frameId){@autoreleasepool{try{
    auto p=(Presenter*)presenter;check(p&&p->overlay,"Native overlay is not attached");
    // Uploads own separate immutable image slots. They must not compete for
    // the producer's frame lock, which may be held across drawable acquisition.
    // Native frames and Canvas redraws sample it through the overlay's own lock.
    if(frameId!=p->uploadedUi){
        int status=klj_overlay_submit(env,p->overlay,pixels,offset,stride,width,height,frameId);
        if(!status)p->uploadedUi=frameId;else if(status!=3)return status;
    }
    // UI updates never wait for an in-flight native producer. CanvasFrameSource
    // retains its newest image and retries on the existing window-owner pump.
    std::unique_lock lock(p->frameMutex,std::try_to_lock);if(!lock.owns_lock())return 3;
    if(p->surface||p->projection){
        if(frameId!=p->uploadedUi)return 3;
        check(p->shared&&p->submitShared&&p->queue,"Native UI requires the common graphics presenter");
        auto commands=[p->queue commandBuffer];check(commands,"Native UI command buffer is unavailable");commands.label=@"Android UI common presentation";
        KLJOverlayFrame overlayFrame{};int status=klj_overlay_encode_cached(env,p->overlay,(__bridge void*)commands,&overlayFrame);if(status)return status;
        if(!overlayFrame.texture)return 0;
        FrameMeasurement measurement{p,p->overlayObservation,"overlay"};
        RSPresenterSubmission submission{};submission.size=sizeof(submission);submission.version=RS_PRESENTER_ABI_VERSION;
        submission.commandBuffer=(__bridge void*)commands;submission.sourceTexture=overlayFrame.texture;submission.frameId=++p->serial;submission.sourceTime=CACurrentMediaTime();
        RSFrameResult result{};result.size=sizeof(result);result.version=RS_FRAME_ABI_VERSION;
        status=submitFrame(p,&submission,nullptr,&result);if(status==RS_FRAME_BUSY){++p->busy;return 3;}
        check(status==RS_FRAME_OK&&result.encoded,*result.reason?result.reason:"Native UI presentation failed");
        klj_overlay_frame_submitted(p->overlay,&overlayFrame);
        [commands commit];p->last=commands;++p->rendered;++p->accepted;return 0;
    }
    FrameMeasurement measurement{p,p->canvasObservation,"canvas"};
    // The same Apple Silicon device is used by the Canvas upload and ANGLE's
    // later native producer; no second window, presenter or polling loop exists.
    if(!p->deviceReady)initializeDevice(p,p->layer.device?:MTLCreateSystemDefaultDevice(),true);
    else initializeShared(p,true);
    check(p->shared&&p->submitShared,"Canvas requires the common graphics presenter");
    check(!p->last||p->last.status!=MTLCommandBufferStatusError,p->last.error.localizedDescription.UTF8String?:"Prior Canvas presentation failed");
    p->width=width;p->height=height;
    int status=klj_overlay_set_surface(env,p->overlay,(__bridge void*)p->device,width,height);if(status)return status;
    if(frameId!=p->uploadedUi){status=klj_overlay_submit(env,p->overlay,pixels,offset,stride,width,height,frameId);if(status)return status;p->uploadedUi=frameId;}
    auto commands=[p->queue commandBuffer];check(commands,"Canvas command buffer is unavailable");commands.label=@"Android Canvas common presentation";
    void *source=nullptr;status=klj_overlay_encode_canvas(env,p->overlay,(__bridge void*)commands,&source);if(status)return status;
    RSPresenterSubmission submission{};submission.size=sizeof(submission);submission.version=RS_PRESENTER_ABI_VERSION;
    submission.commandBuffer=(__bridge void*)commands;submission.sourceTexture=source;submission.frameId=++p->serial;submission.sourceTime=CACurrentMediaTime();
    RSFrameResult result{};result.size=sizeof(result);result.version=RS_FRAME_ABI_VERSION;
    status=submitFrame(p,&submission,nullptr,&result);if(status==RS_FRAME_BUSY){++p->busy;return 3;}
    check(status==RS_FRAME_OK&&result.encoded,*result.reason?result.reason:"Canvas presentation failed");
    [commands commit];p->last=commands;++p->rendered;++p->accepted;
    p->committedExtent.store((uint64_t(uint32_t(width))<<32)|uint32_t(height),std::memory_order_release);return 0;
}catch(const std::exception&e){fail(env,e.what());return 4;}}}
extern "C" int klj_metal_overlay_clear(JNIEnv*env,void*presenter,uint64_t frameId){auto p=(Presenter*)presenter;if(!p||!p->overlay){fail(env,"Native overlay is not attached");return 4;}return klj_overlay_clear(env,p->overlay,frameId);}

extern "C" void klj_metal_supply_storage(void *storage){if(suppliedStorage)CFRelease(suppliedStorage);suppliedStorage=storage?(IOSurfaceRef)CFRetain(storage):nullptr;}
extern "C" int klj_metal_detach_surface(JNIEnv *env){@autoreleasepool{try{auto p=active;check(p,"No active native presenter");std::lock_guard lock(p->frameMutex);p->copies.retire();removeSync(p,p->producerSync);removeSync(p,p->consumerSync);p->last=nil;p->source=nil;p->producerBuffers={};p->target.retire();p->surface=nullptr;p->rendered=0;p->initialized=false;if(p->storage){CFRelease(p->storage);p->storage=nullptr;}return 1;}catch(const std::exception &error){fail(env,error.what());return 0;}}}

extern "C" uint64_t klj_metal_committed_extent(void *value){auto p=static_cast<Presenter*>(value);return p?p->committedExtent.load(std::memory_order_acquire):0;}
extern "C" unsigned klj_metal_target(void*value,void*display,void*surface,int64_t nanos){auto p=static_cast<Presenter*>(value);if(!p)return 0x300D;if(!p->shared||!p->submitTarget||!p->targetTiming)return 0x3002;RSPresentationTiming timing{};timing.size=sizeof(timing);timing.version=RS_TARGET_ABI_VERSION;if(p->targetTiming(p->shared,&timing)!=RS_FRAME_OK)return 0x3002;return p->target.replace(display,surface,nanos);}
extern "C" int klj_metal_target_timing(void*value,void*result){auto p=static_cast<Presenter*>(value);return p&&p->shared&&p->targetTiming?p->targetTiming(p->shared,static_cast<RSPresentationTiming*>(result)):RS_FRAME_INVALID_ARGUMENT;}
