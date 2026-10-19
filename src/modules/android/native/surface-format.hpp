#pragma once
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

namespace klj_surface_format {
inline bool rgb565(IOSurfaceRef storage){return IOSurfaceGetPixelFormat(storage)==(uint32_t)'L565';}
inline MTLPixelFormat metal(IOSurfaceRef storage){
    if(rgb565(storage))return MTLPixelFormatB5G6R5Unorm;
    return IOSurfaceGetPixelFormat(storage)==(uint32_t)'RGBA'?MTLPixelFormatRGBA8Unorm:MTLPixelFormatBGRA8Unorm;
}
inline id<MTLTexture> texture(id<MTLDevice> device,IOSurfaceRef storage){
    if(!device||!storage)return nil;
    auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metal(storage)
        width:IOSurfaceGetWidth(storage) height:IOSurfaceGetHeight(storage) mipmapped:NO];
    descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
    descriptor.storageMode=MTLStorageModeShared;
    return [device newTextureWithDescriptor:descriptor iosurface:storage plane:0];
}
inline int glType(IOSurfaceRef storage){return rgb565(storage)?0x8363:0x1401;}
inline int glFormat(IOSurfaceRef storage,bool opaque=false){
    if(rgb565(storage)||opaque)return 0x1907;
    return IOSurfaceGetPixelFormat(storage)==(uint32_t)'RGBA'?0x1908:0x80E1;
}
}
