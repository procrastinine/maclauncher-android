#include <jni.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "presentation.h"
#include <cstdint>
#include <cstring>
extern "C" void *klj_hardwarebuffer_create(CVPixelBufferRef);
extern "C" void klj_hardwarebuffer_release(void*);
extern "C" void *kl_mediandk_buffer_pixels(const void*);
extern "C" void *klj_surface_acquire_lease(uint64_t,void**,int*,int*);
extern "C" void klj_surface_release_lease(void*);
namespace {
CVPixelBufferRef pixels(jlong handle){return (CVPixelBufferRef)kl_mediandk_buffer_pixels((void*)(uintptr_t)handle);}
void error(JNIEnv *env,const char *message){jclass type=env->FindClass("java/lang/IllegalStateException");if(type){env->ThrowNew(type,message);env->DeleteLocalRef(type);}}
void registerRGBA(){
    static dispatch_once_t once;
    dispatch_once(&once,^{
        // macOS declares 32RGBA but does not supply its CoreVideo allocation
        // description. Register the actual Android byte layout, retaining RGBA
        // planes for CPU clients and direct RGBA8Unorm IOSurface imports.
        auto existing=CVPixelFormatDescriptionCreateWithPixelFormatType(kCFAllocatorDefault,kCVPixelFormatType_32RGBA);
        if(existing){CFRelease(existing);return;}
        NSDictionary *format=@{(id)kCVPixelFormatName:@"Android RGBA",
            (id)kCVPixelFormatConstant:@(kCVPixelFormatType_32RGBA),
            (id)kCVPixelFormatBitsPerBlock:@32,(id)kCVPixelFormatBlockWidth:@1,(id)kCVPixelFormatBlockHeight:@1,
            (id)kCVPixelFormatContainsAlpha:@YES,(id)kCVPixelFormatContainsRGB:@YES,
            (id)kCVPixelFormatComponentRange:(id)kCVPixelFormatComponentRange_FullRange};
        CVPixelFormatDescriptionRegisterDescriptionWithPixelFormatType((__bridge CFDictionaryRef)format,kCVPixelFormatType_32RGBA);
    });
}
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_createStorage(JNIEnv *env,jclass,jint width,jint height){@autoreleasepool{
    registerRGBA();
    NSDictionary *properties=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{},(id)kCVPixelBufferMetalCompatibilityKey:@YES};
    CVPixelBufferRef buffer=nullptr;auto result=CVPixelBufferCreate(kCFAllocatorDefault,width,height,kCVPixelFormatType_32RGBA,(__bridge CFDictionaryRef)properties,&buffer);
    if(result!=kCVReturnSuccess||!buffer){char message[96];snprintf(message,sizeof(message),"Hardware buffer allocation failed (CoreVideo %d)",result);error(env,message);return 0;}
    result=CVPixelBufferLockBaseAddress(buffer,0);if(result!=kCVReturnSuccess){CVPixelBufferRelease(buffer);error(env,"Hardware buffer storage cannot be mapped");return 0;}
    // Zero newly allocated storage, including row padding, before exposing it.
    memset(CVPixelBufferGetBaseAddress(buffer),0,CVPixelBufferGetDataSize(buffer));
    void *owner=klj_hardwarebuffer_create(buffer);
    if(!owner){CVPixelBufferUnlockBaseAddress(buffer,0);error(env,"Hardware buffer ownership allocation failed");}
    CVPixelBufferRelease(buffer);return (jlong)(uintptr_t)owner;
}}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_releaseStorage(JNIEnv*,jclass,jlong handle){
    if(auto buffer=pixels(handle))CVPixelBufferUnlockBaseAddress(buffer,0);klj_hardwarebuffer_release((void*)(uintptr_t)handle);
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_surfaceStorage(JNIEnv*,jclass,jlong handle){auto buffer=pixels(handle);return buffer?(jlong)(uintptr_t)CVPixelBufferGetIOSurface(buffer):0;}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storageRowStride(JNIEnv*,jclass,jlong handle){auto buffer=pixels(handle);return buffer?(jint)CVPixelBufferGetBytesPerRow(buffer):0;}
extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storagePlane(JNIEnv *env,jclass,jlong handle){auto buffer=pixels(handle);return buffer?env->NewDirectByteBuffer(CVPixelBufferGetBaseAddress(buffer),CVPixelBufferGetDataSize(buffer)):nullptr;}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storagePixels(JNIEnv *env,jclass,jlong handle,jintArray array,jboolean write,jboolean opaque){
    auto buffer=pixels(handle);if(!buffer){error(env,"Hardware buffer has closed");return;}
    size_t width=CVPixelBufferGetWidth(buffer),height=CVPixelBufferGetHeight(buffer),stride=CVPixelBufferGetBytesPerRow(buffer);
    if(width*height>(size_t)env->GetArrayLength(array)){error(env,"Hardware buffer pixel array is too small");return;}
    auto colors=env->GetIntArrayElements(array,nullptr);if(!colors)return;
    auto bytes=(uint8_t*)CVPixelBufferGetBaseAddress(buffer);
    for(size_t y=0;y<height;y++)for(size_t x=0;x<width;x++){
        auto pixel=bytes+y*stride+x*4;auto &color=colors[y*width+x];
        if(write){pixel[0]=(uint32_t)color>>16;pixel[1]=(uint32_t)color>>8;pixel[2]=color;pixel[3]=opaque?255:(uint32_t)color>>24;}
        else color=((uint32_t)(opaque?255:pixel[3])<<24)|((uint32_t)pixel[0]<<16)|((uint32_t)pixel[1]<<8)|pixel[2];
    }
    env->ReleaseIntArrayElements(array,colors,write?JNI_ABORT:0);
}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_presentStorage(JNIEnv *env,jclass,jlong handle,jlong lease){@autoreleasepool{
    auto buffer=pixels(handle);if(!buffer)return -19;
    void *presenter=nullptr;int width=0,height=0;void *window=klj_surface_acquire_lease(lease,&presenter,&width,&height);
    if(!window)return -19;
    struct Scope {void *window;~Scope(){klj_surface_release_lease(window);}} scope{window};
    if(!presenter)return -38;
    if(width!=(int)CVPixelBufferGetWidth(buffer)||height!=(int)CVPixelBufferGetHeight(buffer))return -22;
    auto device=MTLCreateSystemDefaultDevice();auto queue=(__bridge id<MTLCommandQueue>)klj_metal_projection_queue(env,presenter,(__bridge void*)device);if(env->ExceptionCheck()||!queue)return -19;
    auto description=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];description.storageMode=MTLStorageModeShared;description.usage=MTLTextureUsageShaderRead;
    auto texture=[device newTextureWithDescriptor:description iosurface:CVPixelBufferGetIOSurface(buffer) plane:0];if(!texture){error(env,"Hardware buffer cannot be imported by Metal");return -22;}
    auto commands=[queue commandBuffer];commands.label=@"Android hardware buffer presentation";
    int result=klj_metal_project(env,presenter,nullptr,(__bridge void*)commands,(__bridge void*)texture);if(!result)[commands commit];
    return result==0?0:result==3?-11:-22;
}}
