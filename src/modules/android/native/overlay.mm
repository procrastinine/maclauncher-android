#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#include "overlay.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <algorithm>

namespace {
constexpr uint64_t budget=192ull*1024*1024;
constexpr int ok=0,busy=3,failed=4;
// Includes completion-owned images after a window owner has been destroyed.
std::atomic<uint64_t> globalBytes{0};
std::atomic<unsigned> globalUiSlots{0},globalOutputSlots{0},globalBaseSlots{0};
bool reserveBytes(uint64_t bytes){auto used=globalBytes.load();do{if(bytes>budget-used)return false;}while(!globalBytes.compare_exchange_weak(used,used+bytes));return true;}
struct Counts {
    std::atomic<uint64_t> bytes{0},peak{0},uploaded{0},busy{0},composed{0},bypassed{0},cleared{0};
    std::atomic<unsigned> ui{0},outputs{0},bases{0},flight{0};
};
struct Image {
    id<MTLTexture> texture;
    IOSurfaceRef storage=nullptr;
    unsigned width=0,height=0;
    uint64_t bytes=0;
    uint64_t reservation=0;
    bool reservedSlot=false;
    bool ui=false,base=false;
    std::shared_ptr<Counts> counts;
    ~Image(){texture=nil;if(storage)CFRelease(storage);if(bytes){counts->bytes-=bytes;if(ui)counts->ui--;else if(base)counts->bases--;else counts->outputs--;}if(reservation)globalBytes-=reservation;if(reservedSlot){if(ui)globalUiSlots--;else if(base)globalBaseSlots--;else globalOutputSlots--;}}
};
struct Lease {
    std::shared_ptr<Image> ui,output,base;
    id<MTLTexture> source;
    std::shared_ptr<Counts> counts;
    std::atomic<bool> retired{false};
    Lease(std::shared_ptr<Image> u,std::shared_ptr<Image> o,id<MTLTexture> s,std::shared_ptr<Counts> c):ui(std::move(u)),output(std::move(o)),source(s),counts(std::move(c)){counts->flight++;}
    void retire(){if(!retired.exchange(true)){ui.reset();output.reset();base.reset();source=nil;counts->flight--;}}
    ~Lease(){retire();}
};
struct State {
    std::mutex mutex;
    id<MTLDevice> device;
    std::atomic<void*> validatedDevice{nullptr};
    std::array<id<MTLRenderPipelineState>,3> pipelines;
    std::array<std::shared_ptr<Image>,3> ui,outputs,bases;
    std::shared_ptr<Image> latest,base;
    std::shared_ptr<Counts> counts=std::make_shared<Counts>();
    unsigned width,height;
    uint64_t lastUiId=0,sourceGeneration=1,captureSerial=0,baseSerial=0,presentedUiId=0;
    bool baseTopLeft=false,presentedVisible=false;
    explicit State(unsigned w,unsigned h):width(w),height(h){}
};
using Owner=std::shared_ptr<State>;
void check(bool value,const char*why){if(!value)throw std::runtime_error(why);}
void fail(JNIEnv*env,const char*why){if(!env||env->ExceptionCheck())return;auto c=env->FindClass("java/lang/IllegalStateException");if(c){env->ThrowNew(c,why);env->DeleteLocalRef(c);}}
Owner state(void*p){check(p,"Overlay owner is unavailable");return *static_cast<Owner*>(p);}
bool dimensions(int w,int h){return w>0&&h>0&&w<=8192&&h<=8192&&uint64_t(w)*unsigned(h)<=16777216;}
uint64_t rowBytes(unsigned w){return IOSurfaceAlignProperty(kIOSurfaceBytesPerRow,uint64_t(w)*4);}
uint64_t surfaceBytes(unsigned w,unsigned h){return IOSurfaceAlignProperty(kIOSurfaceAllocSize,rowBytes(w)*h);}
MTLTextureDescriptor *descriptor(unsigned w,unsigned h,MTLPixelFormat format,bool ui,bool base=false){
    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    d.storageMode=ui?MTLStorageModeShared:MTLStorageModePrivate;d.usage=ui||base?MTLTextureUsageShaderRead:(MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);return d;
}
uint64_t textureBytes(id<MTLDevice> device,unsigned w,unsigned h,MTLPixelFormat format,bool base=false){return [device heapTextureSizeAndAlignWithDescriptor:descriptor(w,h,format,false,base)].size;}
void validateDevice(id<MTLDevice> device,int w,int h){
    check(dimensions(w,h),"Overlay dimensions exceed the supported range");
    uint64_t ui=surfaceBytes(w,h),out=device?std::max(textureBytes(device,w,h,MTLPixelFormatBGRA8Unorm),textureBytes(device,w,h,MTLPixelFormatRGBA8Unorm)):ui;
    check(ui<=budget/2&&out<=budget-2*ui,"Overlay extent cannot fit two UI snapshots and one composite");
}
void validate(State&s,int w,int h){validateDevice(s.device,w,h);}
void charge(Image&i,uint64_t bytes){i.bytes=bytes;auto now=i.counts->bytes.fetch_add(bytes)+bytes;if(i.ui)i.counts->ui++;else if(i.base)i.counts->bases++;else i.counts->outputs++;auto peak=i.counts->peak.load();while(now>peak&&!i.counts->peak.compare_exchange_weak(peak,now)){};}
void detachPools(State&s){for(auto*pool:{&s.ui,&s.outputs})for(auto&image:*pool)image.reset();}
void trimIdleToFit(State&s,uint64_t need){
    if(need<=budget-s.counts->bytes.load())return;
    for(auto*pool:{&s.ui,&s.outputs,&s.bases})for(auto&image:*pool)if(image&&image.use_count()==1){image.reset();if(need<=budget-s.counts->bytes.load())return;}
}
std::shared_ptr<Image> acquire(State&s,bool ui,MTLPixelFormat format,unsigned width,unsigned height,bool base=false){
    auto&pool=ui?s.ui:base?s.bases:s.outputs;
    for(auto&image:pool)if(image&&image.use_count()==1&&image->width==width&&image->height==height&&image->texture.pixelFormat==format)return image;
    for(auto&image:pool)if(image&&image.use_count()==1&&(image->width!=width||image->height!=height))image.reset();
    auto spot=std::find_if(pool.begin(),pool.end(),[](auto&i){return !i||i.use_count()==1;});if(spot==pool.end())return {};
    // The slot itself may contribute bytes; release it before affordability.
    spot->reset();uint64_t need=ui?surfaceBytes(width,height):textureBytes(s.device,width,height,format,base);
    trimIdleToFit(s,need);if(need>budget-s.counts->bytes.load())return {};
    auto image=std::make_shared<Image>();image->ui=ui;image->base=base;image->width=width;image->height=height;image->counts=s.counts;
    auto&globalSlots=ui?globalUiSlots:base?globalBaseSlots:globalOutputSlots;if(globalSlots.fetch_add(1)>=3){globalSlots--;return {};}image->reservedSlot=true;
    if(!reserveBytes(need))return {};image->reservation=need;
    if(ui){
        NSDictionary*properties=@{(__bridge NSString*)kIOSurfaceWidth:@(width),(__bridge NSString*)kIOSurfaceHeight:@(height),(__bridge NSString*)kIOSurfaceBytesPerElement:@4,(__bridge NSString*)kIOSurfaceBytesPerRow:@(rowBytes(width)),(__bridge NSString*)kIOSurfaceAllocSize:@(need),(__bridge NSString*)kIOSurfacePixelFormat:@((uint32_t)'BGRA')};
        image->storage=IOSurfaceCreate((__bridge CFDictionaryRef)properties);check(image->storage,"Overlay IOSurface allocation failed");need=IOSurfaceGetAllocSize(image->storage);
        image->texture=[s.device newTextureWithDescriptor:descriptor(width,height,format,true) iosurface:image->storage plane:0];
    }else image->texture=[s.device newTextureWithDescriptor:descriptor(width,height,format,false,base)];
    check(image->texture,"Overlay texture allocation failed");
    if(!ui)need=std::max(need,uint64_t(image->texture.allocatedSize));
    // heapTextureSizeAndAlign estimates heap placement, not a standalone
    // texture's allocation. Charge Metal's actual footprint before publishing
    // the image, including alignment changes at non-power-of-two extents.
    trimIdleToFit(s,need);if(need>budget-s.counts->bytes.load())return {};
    if(need>image->reservation){if(!reserveBytes(need-image->reservation))return {};image->reservation=need;}
    if(need<image->reservation){globalBytes-=image->reservation-need;image->reservation=need;}charge(*image,need);*spot=image;return image;
}
const char*shader=R"METAL(
#include <metal_stdlib>
using namespace metal;
vertex float4 overlayVertex(uint id [[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[id],0,1);}
float4 overlayPixel(texture2d<float> ui,int2 p){
    float4 color=ui.read(uint2(clamp(p,int2(0),int2(ui.get_width()-1,ui.get_height()-1))));
    return float4(color.rgb*color.a,color.a);
}
fragment float4 overlayFragment(float4 position [[position]],texture2d<float> source [[texture(0)]],texture2d<float> ui [[texture(1)]],constant uint &topLeft [[buffer(0)]]){
    uint2 p=uint2(position.xy);float4 base=source.read(p),top;
    if(source.get_width()==ui.get_width()&&source.get_height()==ui.get_height())top=overlayPixel(ui,int2(p.x,topLeft?p.y:ui.get_height()-1-p.y));
    else{
        // Canvas layout and a game's render target are independent extents.
        // Interpolate premultiplied colors to retain transparent edge colors.
        float2 uv=position.xy/float2(source.get_width(),source.get_height());if(!topLeft)uv.y=1-uv.y;
        float2 pixel=uv*float2(ui.get_width(),ui.get_height())-.5f;
        int2 origin=int2(floor(pixel));float2 weight=fract(pixel);
        top=mix(mix(overlayPixel(ui,origin),overlayPixel(ui,origin+int2(1,0)),weight.x),
                mix(overlayPixel(ui,origin+int2(0,1)),overlayPixel(ui,origin+int2(1,1)),weight.x),weight.y);
    }
    float a=top.a+base.a*(1-top.a);float3 numerator=top.rgb+base.rgb*base.a*(1-top.a);
    return float4(a>0?numerator/a:float3(0),a);
}
fragment float4 overlayCopyFragment(float4 position [[position]],texture2d<float> source [[texture(0)]]){
    return source.read(uint2(position.xy));
}
)METAL";
void prepare(State&s){
    NSError*error=nil;auto library=[s.device newLibraryWithSource:@(shader) options:nil error:&error];check(library,error.localizedDescription.UTF8String?:"Overlay shader compilation failed");
    for(unsigned i=0;i<3;i++){auto d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[library newFunctionWithName:@"overlayVertex"];d.fragmentFunction=[library newFunctionWithName:i==2?@"overlayCopyFragment":@"overlayFragment"];d.colorAttachments[0].pixelFormat=i==1?MTLPixelFormatRGBA8Unorm:MTLPixelFormatBGRA8Unorm;s.pipelines[i]=[s.device newRenderPipelineStateWithDescriptor:d error:&error];check(s.pipelines[i],error.localizedDescription.UTF8String?:"Overlay pipeline creation failed");}
}
// A producer may recycle its drawable immediately after presentation. Cache a
// GPU-only base while Android UI exists, and publish only completed copies.
// Pool leases prevent reuse while a later UI command buffer samples the copy.
void captureBase(Owner p,id<MTLCommandBuffer> commands,id<MTLTexture> input,bool topLeft){
    if(!p->latest)return;
    auto image=acquire(*p,false,input.pixelFormat,input.width,input.height,true);
    // A completed base with no readers is reusable. Reclaim that optional
    // cache before adding another image, rather than growing the GPU budget.
    if(!image&&p->base&&p->base.use_count()==2){p->base.reset();image=acquire(*p,false,input.pixelFormat,input.width,input.height,true);}
    if(!image)return;
    auto lease=std::make_shared<Lease>(nullptr,image,input,p->counts);
    uint64_t generation=p->sourceGeneration,serial=++p->captureSerial;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> completed){
        if(completed.status==MTLCommandBufferStatusCompleted){std::lock_guard guard(p->mutex);
            if(generation==p->sourceGeneration&&serial>p->baseSerial){p->base=lease->output;p->baseTopLeft=topLeft;p->baseSerial=serial;}}
        lease->retire();
    }];
    auto copy=[commands blitCommandEncoder];check(copy,"Native base copy encoder is unavailable");
    [copy copyFromTexture:input sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(input.width,input.height,1)
                toTexture:image->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[copy endEncoding];
}
}
extern "C" void*klj_overlay_create(JNIEnv*env,int width,int height){@autoreleasepool{try{check(dimensions(width,height),"Invalid overlay extent");auto p=std::make_shared<State>(width,height);validate(*p,width,height);return new Owner(p);}catch(const std::exception&e){fail(env,e.what());return nullptr;}}}
extern "C" int klj_overlay_validate_size(JNIEnv*env,void*owner,int width,int height){@autoreleasepool{try{auto p=state(owner);auto device=(__bridge id<MTLDevice>)p->validatedDevice.load();validateDevice(device,width,height);return ok;}catch(const std::exception&e){fail(env,e.what());return failed;}}}
extern "C" int klj_overlay_set_surface(JNIEnv*env,void*owner,void*device,int width,int height){@autoreleasepool{try{
    auto p=state(owner);std::unique_lock lock(p->mutex,std::try_to_lock);if(!lock.owns_lock()){p->counts->busy++;return busy;}
    check(device,"Overlay Metal device is unavailable");auto next=(__bridge id<MTLDevice>)device;check(!p->device||p->device==next,"Overlay cannot change Metal devices");
    if(p->validatedDevice.load()&&p->width==unsigned(width)&&p->height==unsigned(height))return ok;
    if(!p->validatedDevice.load()){p->device=next;validate(*p,width,height);prepare(*p);p->validatedDevice=(__bridge void*)next;}else validate(*p,width,height);
    // The producer owns only its output extent. Retain the last complete UI
    // snapshot through native resize/orientation until Canvas replaces it.
    if(p->width!=unsigned(width)||p->height!=unsigned(height)){for(auto*pool:{&p->outputs,&p->bases})for(auto&image:*pool)image.reset();p->base.reset();p->sourceGeneration++;p->width=width;p->height=height;}
    return ok;
}catch(const std::exception&e){fail(env,e.what());return failed;}}}
extern "C" int klj_overlay_submit(JNIEnv*env,void*owner,jintArray pixels,int offset,int stride,int width,int height,uint64_t frameId){@autoreleasepool{try{
    auto p=state(owner);check(env&&pixels&&dimensions(width,height)&&offset>=0&&stride>=width,"Invalid overlay pixel array");
    check(uint64_t(offset)+uint64_t(height-1)*unsigned(stride)+unsigned(width)<=uint64_t(env->GetArrayLength(pixels)),"Overlay pixel array is too small");
    std::unique_lock lock(p->mutex,std::try_to_lock);if(!lock.owns_lock()){p->counts->busy++;return busy;}
    check(frameId>p->lastUiId,"Overlay UI frame identity was already accepted");validate(*p,width,height);
    if(!p->device){p->counts->busy++;return busy;}
    auto image=acquire(*p,true,MTLPixelFormatBGRA8Unorm,width,height);if(!image){p->counts->busy++;return busy;}
    check(IOSurfaceLock(image->storage,kIOSurfaceLockAvoidSync,nullptr)==kIOReturnSuccess,"Overlay upload storage lock failed");
    bool visible=false;auto bytes=static_cast<unsigned char*>(IOSurfaceGetBaseAddress(image->storage));auto row=IOSurfaceGetBytesPerRow(image->storage);
    static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__,"Java ARGB requires little-endian BGRA upload");
    for(int y=0;y<height&&!env->ExceptionCheck();y++){auto dst=reinterpret_cast<jint*>(bytes+y*row);env->GetIntArrayRegion(pixels,offset+y*stride,width,dst);if(!env->ExceptionCheck())for(int x=0;x<width&&!visible;x++)visible=(uint32_t(dst[x])>>24)!=0;}
    auto unlocked=IOSurfaceUnlock(image->storage,kIOSurfaceLockAvoidSync,nullptr);if(env->ExceptionCheck())return failed;check(unlocked==kIOReturnSuccess,"Overlay upload storage unlock failed");
    p->counts->uploaded++;p->lastUiId=frameId;p->latest=visible?image:nullptr;
    if(!visible){p->counts->cleared++;image.reset();detachPools(*p);}return ok;
}catch(const std::exception&e){fail(env,e.what());return failed;}}}
extern "C" int klj_overlay_clear(JNIEnv*env,void*owner,uint64_t frameId){@autoreleasepool{try{
    auto p=state(owner);std::unique_lock lock(p->mutex,std::try_to_lock);if(!lock.owns_lock()){p->counts->busy++;return busy;}check(frameId>p->lastUiId,"Overlay UI frame identity was already accepted");p->lastUiId=frameId;p->latest.reset();p->counts->cleared++;detachPools(*p);return ok;
}catch(const std::exception&e){fail(env,e.what());return failed;}}}
static int encode(JNIEnv*env,void*owner,void*buffer,void*source,int sourceTopLeft,KLJOverlayFrame*output,std::shared_ptr<Image> retainedBase={},bool capture=true){@autoreleasepool{try{
    check(output,"Overlay output pointer is unavailable");*output={};auto p=state(owner);auto commands=(__bridge id<MTLCommandBuffer>)buffer;auto input=(__bridge id<MTLTexture>)source;
    check(commands&&input&&commands.retainedReferences&&commands.status==MTLCommandBufferStatusNotEnqueued,"Overlay requires a retained unsubmitted command buffer");
    std::unique_lock lock(p->mutex,std::try_to_lock);if(!lock.owns_lock()){p->counts->busy++;return busy;}
    check(p->device&&commands.device==p->device&&input.device==p->device,"Overlay source device mismatch");
    check(input.width==p->width&&input.height==p->height&&input.textureType==MTLTextureType2D&&input.sampleCount==1,"Overlay source dimensions or samples mismatch");
    output->uiFrameId=p->lastUiId;output->uiVisible=bool(p->latest);
    // Native framebuffer precision remains unchanged. Sample its actual Metal
    // format and normalize only the presentation output (including RGB565).
    const bool direct=input.pixelFormat==MTLPixelFormatBGRA8Unorm||input.pixelFormat==MTLPixelFormatRGBA8Unorm;
    if(!p->latest&&direct){if(retainedBase){auto lease=std::make_shared<Lease>(nullptr,nullptr,input,p->counts);lease->base=std::move(retainedBase);[commands addCompletedHandler:^(id<MTLCommandBuffer>){lease->retire();}];}p->counts->bypassed++;output->texture=source;return ok;}
    if(p->latest)check(input!=p->latest->texture&&(!input.iosurface||!p->latest->storage||IOSurfaceGetID(input.iosurface)!=IOSurfaceGetID(p->latest->storage)),"Native source aliases the Canvas overlay");
    const auto format=direct?input.pixelFormat:MTLPixelFormatBGRA8Unorm;
    auto composite=acquire(*p,false,format,p->width,p->height);
    if(!composite&&!retainedBase&&p->base){p->base.reset();composite=acquire(*p,false,format,p->width,p->height);}
    if(!composite){p->counts->busy++;return busy;}
    // Native composition has priority over the optional immutable snapshot.
    // Cache allocation must never steal the output needed for a game frame.
    if(capture)captureBase(p,commands,input,sourceTopLeft!=0);
    auto lease=std::make_shared<Lease>(p->latest,composite,input,p->counts);lease->base=std::move(retainedBase);auto pipeline=p->pipelines[p->latest?(format==MTLPixelFormatRGBA8Unorm?1:0):2];
    [commands addCompletedHandler:^(id<MTLCommandBuffer>){lease->retire();}];lock.unlock();
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=composite->texture;pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    auto encoder=[commands renderCommandEncoderWithDescriptor:pass];check(encoder,"Overlay encoder is unavailable");[encoder setRenderPipelineState:pipeline];[encoder setFragmentTexture:input atIndex:0];if(lease->ui){[encoder setFragmentTexture:lease->ui->texture atIndex:1];uint32_t topLeft=sourceTopLeft!=0;[encoder setFragmentBytes:&topLeft length:sizeof(topLeft) atIndex:0];}[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    p->counts->composed++;output->texture=(__bridge void*)composite->texture;return ok;
}catch(const std::exception&e){fail(env,e.what());return failed;}}}
extern "C" int klj_overlay_encode(JNIEnv*env,void*owner,void*buffer,void*source,int sourceTopLeft,KLJOverlayFrame*output){return encode(env,owner,buffer,source,sourceTopLeft,output);}
extern "C" int klj_overlay_encode_cached(JNIEnv*env,void*owner,void*buffer,KLJOverlayFrame*output){@autoreleasepool{try{
    check(output,"Overlay output pointer is unavailable");*output={};auto p=state(owner);std::unique_lock lock(p->mutex,std::try_to_lock);if(!lock.owns_lock())return busy;
    auto base=p->base;bool topLeft=p->baseTopLeft;if(!base)return !p->latest&&!p->presentedVisible?ok:busy;
    lock.unlock();auto texture=(__bridge void*)base->texture;return encode(env,owner,buffer,texture,topLeft,output,std::move(base),false);
}catch(const std::exception&e){fail(env,e.what());return failed;}}}
extern "C" void klj_overlay_frame_submitted(void*owner,const KLJOverlayFrame*frame){
    if(!owner||!frame||!frame->texture)return;auto p=state(owner);std::lock_guard lock(p->mutex);
    if(frame->uiFrameId>=p->presentedUiId){p->presentedUiId=frame->uiFrameId;p->presentedVisible=frame->uiVisible!=0;}
    if(!frame->uiVisible&&frame->uiFrameId==p->lastUiId&&!p->latest){
        p->base.reset();p->sourceGeneration++;
        for(auto&image:p->bases)image.reset();
    }
}
extern "C" int klj_overlay_stats(void*owner,KLJOverlayStats*out){if(!owner||!out)return failed;auto p=state(owner);std::unique_lock lock(p->mutex,std::try_to_lock);if(!lock.owns_lock())return busy;auto c=p->counts;*out={c->bytes,c->peak,c->uploaded,c->busy,c->composed,c->bypassed,c->cleared,p->lastUiId,c->ui,c->outputs,c->flight,c->bases};return ok;}
extern "C" int klj_overlay_encode_canvas(JNIEnv*env,void*owner,void*buffer,void**output){@autoreleasepool{try{
    check(output,"Canvas output pointer is unavailable");*output=nullptr;auto p=state(owner);auto commands=(__bridge id<MTLCommandBuffer>)buffer;
    check(commands&&commands.retainedReferences&&commands.status==MTLCommandBufferStatusNotEnqueued,"Canvas requires a retained unsubmitted command buffer");
    std::unique_lock lock(p->mutex,std::try_to_lock);if(!lock.owns_lock()){p->counts->busy++;return busy;}
    check(p->device&&commands.device==p->device,"Canvas source device mismatch");
    auto image=p->latest;const bool clear=!image;
    if(clear)image=acquire(*p,false,MTLPixelFormatBGRA8Unorm,p->width,p->height);
    if(!image){p->counts->busy++;return busy;}
    auto lease=std::make_shared<Lease>(clear?nullptr:image,clear?image:nullptr,nil,p->counts);
    [commands addCompletedHandler:^(id<MTLCommandBuffer>){lease->retire();}];
    if(clear){auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=image->texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);auto encoder=[commands renderCommandEncoderWithDescriptor:pass];check(encoder,"Canvas clear encoder is unavailable");[encoder endEncoding];}
    *output=(__bridge void*)image->texture;return ok;
}catch(const std::exception&e){fail(env,e.what());return failed;}}}
extern "C" void klj_overlay_destroy(void*owner){@autoreleasepool{delete static_cast<Owner*>(owner);}}
