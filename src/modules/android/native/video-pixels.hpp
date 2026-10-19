#pragma once
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
// Texture outputs preserve CVPixelBuffer row and column order. Consumers own
// their existing Android/Metal texture transforms, including display rotation.
id<MTLTexture> klj_video_texture(CVPixelBufferRef,id<MTLDevice>,id<MTLCommandBuffer>);
bool klj_video_render(CVPixelBufferRef,id<MTLDevice>,id<MTLCommandBuffer>,id<MTLTexture>);
bool klj_video_render(CVPixelBufferRef,id<MTLDevice>,id<MTLCommandBuffer>,id<MTLTexture>,CGColorSpaceRef);
bool klj_video_argb(CVPixelBufferRef,void*,size_t);
