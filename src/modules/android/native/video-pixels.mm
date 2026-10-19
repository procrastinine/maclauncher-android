#import <CoreImage/CoreImage.h>
#import <CoreGraphics/CoreGraphics.h>
#include "video-pixels.hpp"
namespace {
CIContext* context(id<MTLDevice> device){
    static CIContext* shared;static dispatch_once_t once;
    dispatch_once(&once,^{shared=[CIContext contextWithMTLDevice:device options:@{kCIContextCacheIntermediates:@NO}];});return shared;
}
CGColorSpaceRef colorSpace(){static CGColorSpaceRef value;static dispatch_once_t once;dispatch_once(&once,^{value=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);});return value;}
}
id<MTLTexture> klj_video_texture(CVPixelBufferRef pixels,id<MTLDevice> device,id<MTLCommandBuffer> commands){
    if(!pixels||!device||!commands)return nil;
    size_t width=CVPixelBufferGetWidth(pixels),height=CVPixelBufferGetHeight(pixels);
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageRenderTarget;
    auto texture=[device newTextureWithDescriptor:descriptor];if(!texture)return nil;
    if(!klj_video_render(pixels,device,commands,texture))return nil;
    return texture;
}
bool klj_video_render(CVPixelBufferRef pixels,id<MTLDevice> device,id<MTLCommandBuffer> commands,id<MTLTexture> texture){
    return klj_video_render(pixels,device,commands,texture,colorSpace());
}
bool klj_video_render(CVPixelBufferRef pixels,id<MTLDevice> device,id<MTLCommandBuffer> commands,id<MTLTexture> texture,CGColorSpaceRef output){
    if(!pixels||!device||!commands||!texture||texture.width!=CVPixelBufferGetWidth(pixels)||texture.height!=CVPixelBufferGetHeight(pixels))return false;
    auto image=[CIImage imageWithCVPixelBuffer:pixels];
    // CoreImage's Metal destination uses a bottom-left image origin. Android
    // native buffers and the shared texture owner preserve the producer's row
    // zero. Fuse this coordinate conversion into the existing color render.
    image=[image imageByApplyingTransform:CGAffineTransformMake(1,0,0,-1,0,texture.height)];
    [context(device) render:image toMTLTexture:texture commandBuffer:commands bounds:CGRectMake(0,0,texture.width,texture.height) colorSpace:output?:colorSpace()];return true;
}
bool klj_video_argb(CVPixelBufferRef pixels,void*bytes,size_t row){
    if(!pixels||!bytes)return false;auto image=[CIImage imageWithCVPixelBuffer:pixels];
    [context(MTLCreateSystemDefaultDevice()) render:image toBitmap:bytes rowBytes:row bounds:CGRectMake(0,0,CVPixelBufferGetWidth(pixels),CVPixelBufferGetHeight(pixels)) format:kCIFormatBGRA8 colorSpace:colorSpace()];return true;
}
