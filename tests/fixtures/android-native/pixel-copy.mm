#include "pixel-copy.hpp"
#include <dispatch/dispatch.h>
#include <atomic>
namespace {
std::atomic<unsigned> sourceRetirements{0};
struct SourceLease {
    id<MTLTexture> image;
    explicit SourceLease(id<MTLTexture> value):image(value){}
    ~SourceLease(){
        // Returning a real pooled allocation permits its next producer to
        // overwrite it. A pending read must retain the lease, not just texture.
        uint8_t replacement[16]={0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255};
        [image replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:replacement bytesPerRow:8];
        ++sourceRetirements;
    }
};
void retainedSource(JNIEnv*env,jobject callback,int mode){
    auto device=MTLCreateSystemDefaultDevice();auto producer=[device newCommandQueue];
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
    auto source=[device newTextureWithDescriptor:descriptor];
    uint8_t rgba[]={255,0,0,128,0,255,0,255,0,0,255,64,255,255,255,255};
    [source replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:rgba bytesPerRow:8];
    auto queue=std::make_shared<klj_copy::Queue>();auto commands=[producer commandBuffer];
    auto ready=[device newSharedEvent];
    if(mode==9)[commands encodeWaitForEvent:ready value:1];
    if(mode==10)queue->snapshot(commands,source,true);
    else queue->source(commands,source,true,std::make_shared<SourceLease>(source));
    [commands commit];
    if(mode==8||mode==10)[commands waitUntilCompleted];
    if(mode==10){
        // The mutable producer recycles its original allocation while idle.
        // PixelCopy must use the snapshot encoded before its producer fence.
        uint8_t replacement[16]={0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255};
        [source replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:replacement bytesPerRow:8];
    }
    queue->add(env,callback,2,2);
    if(mode==9){
        // Replacing the latest source while its old copy waits must retain the
        // old allocation until producer completion AND the admitted readback.
        auto newer=[device newTextureWithDescriptor:descriptor];
        auto next=[[device newCommandQueue] commandBuffer];
        queue->source(next,newer,true,std::make_shared<SourceLease>(newer));
        [next commit];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,50*NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{ready.signaledValue=1;});
    }
    // No future producer frame is supplied, and both frame/cache owners retire.
}
}
extern "C" JNIEXPORT jint JNICALL Java_PixelCopyProbe_sourceRetirements(JNIEnv*,jclass){return sourceRetirements.load();}
extern "C" JNIEXPORT void JNICALL Java_PixelCopyProbe_copy(JNIEnv *env,jclass,jobject callback,jint mode){@autoreleasepool{
    if(mode==8||mode==9||mode==10){retainedSource(env,callback,mode);return;}
    auto queue=std::make_shared<klj_copy::Queue>();
    auto token=queue->add(env,callback,mode==6||mode==7?1:2,mode==6||mode==7?1:2,mode==1?klj_copy::NoData:-1,0,mode==7?1:0,mode==7?1:0,mode==7?2:0);
    if(mode==1||mode==2)return;
    if(mode==3){dispatch_after(dispatch_time(DISPATCH_TIME_NOW,1500*NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{(void)queue;});return;}
    if(mode==4){klj_copy::cancel(token);return;}
    auto device=MTLCreateSystemDefaultDevice();auto commands=[[device newCommandQueue] commandBuffer];
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mode==5?MTLPixelFormatBGRA8Unorm:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
    auto source=[device newTextureWithDescriptor:descriptor];
    uint8_t rgba[]={255,0,0,128,0,255,0,255,0,0,255,64,255,255,255,255};
    if(mode==5)for(unsigned i=0;i<16;i+=4)std::swap(rgba[i],rgba[i+2]);
    [source replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:rgba bytesPerRow:8];
    queue->encode(commands,source,true);[commands commit];
    // Queue and source owner go away now; actual completion owns readback.
}}
