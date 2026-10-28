#include <jni.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "presentation.h"
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>
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
constexpr OSType rgba1010102='aR10';
OSType pixelFormat(int format){
    switch(format){
        case 1:case 2:case 34:return kCVPixelFormatType_32RGBA;
        case 3:return kCVPixelFormatType_24RGB;
        case 4:return kCVPixelFormatType_16LE565;
        case 22:return kCVPixelFormatType_64RGBAHalf;
        case 43:return rgba1010102;
        case 35:return kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
        case 54:return kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
        case 60:return kCVPixelFormatType_422YpCbCr10BiPlanarFullRange;
        case 33:case 56:case 0x20203859:return kCVPixelFormatType_OneComponent8;
        case 32:case 0x20363159:return kCVPixelFormatType_OneComponent16;
        default:return 0;
    }
}
bool yuv(OSType type){return type==kCVPixelFormatType_420YpCbCr8BiPlanarFullRange||type==kCVPixelFormatType_420YpCbCr10BiPlanarFullRange||type==kCVPixelFormatType_422YpCbCr10BiPlanarFullRange;}
bool tenBit(OSType type){return type==kCVPixelFormatType_420YpCbCr10BiPlanarFullRange||type==kCVPixelFormatType_422YpCbCr10BiPlanarFullRange;}
size_t componentBytes(OSType type){return tenBit(type)?2:1;}
size_t elementBytes(OSType type){
    switch(type){case kCVPixelFormatType_24RGB:return 3;case kCVPixelFormatType_16LE565:case kCVPixelFormatType_OneComponent16:return 2;case kCVPixelFormatType_64RGBAHalf:return 8;case kCVPixelFormatType_OneComponent8:return 1;default:return 4;}
}
void registerFormats(){
    registerRGBA();static dispatch_once_t once;
    dispatch_once(&once,^{
        // Like RGBA, Darwin declares this Android byte layout without a
        // usable CVPixelBuffer allocation description. IOSurface already
        // supplies its native little-endian two-byte RGB565 storage.
        NSDictionary *rgb565=@{(id)kCVPixelFormatName:@"Android RGB565",(id)kCVPixelFormatConstant:@(kCVPixelFormatType_16LE565),
            (id)kCVPixelFormatBitsPerBlock:@16,(id)kCVPixelFormatBlockWidth:@1,(id)kCVPixelFormatBlockHeight:@1,
            (id)kCVPixelFormatContainsAlpha:@NO,(id)kCVPixelFormatContainsRGB:@YES,
            (id)kCVPixelFormatComponentRange:(id)kCVPixelFormatComponentRange_FullRange};
        CVPixelFormatDescriptionRegisterDescriptionWithPixelFormatType((__bridge CFDictionaryRef)rgb565,kCVPixelFormatType_16LE565);
        NSDictionary *format=@{(id)kCVPixelFormatName:@"Android RGBA1010102",(id)kCVPixelFormatConstant:@(rgba1010102),
            (id)kCVPixelFormatBitsPerBlock:@32,(id)kCVPixelFormatBlockWidth:@1,(id)kCVPixelFormatBlockHeight:@1,
            (id)kCVPixelFormatContainsAlpha:@YES,(id)kCVPixelFormatContainsRGB:@YES,
            (id)kCVPixelFormatComponentRange:(id)kCVPixelFormatComponentRange_FullRange};
        CVPixelFormatDescriptionRegisterDescriptionWithPixelFormatType((__bridge CFDictionaryRef)format,rgba1010102);
    });
}
struct Plane {uint8_t *bytes;size_t size,row,pixel;};
bool plane(CVPixelBufferRef buffer,int index,Plane &out){
    if(!buffer||index<0)return false;auto type=CVPixelBufferGetPixelFormatType(buffer);
    if(yuv(type)){
        if(index>2)return false;size_t unit=componentBytes(type),physical=index?1:0,offset=index==2?unit:0;
        size_t row=CVPixelBufferGetBytesPerRowOfPlane(buffer,physical),height=CVPixelBufferGetHeightOfPlane(buffer,physical),width=CVPixelBufferGetWidthOfPlane(buffer,physical),step=index?2*unit:unit;
        size_t size=row*(height-1)+step*(width-1)+unit;
        out={(uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buffer,physical)+offset,size,row,step};return true;
    }
    if(index)return false;out={(uint8_t*)CVPixelBufferGetBaseAddress(buffer),CVPixelBufferGetDataSize(buffer),CVPixelBufferGetBytesPerRow(buffer),elementBytes(type)};return true;
}
int byte(double value){return std::clamp((int)std::lround(value),0,255);}
uint32_t argb(CVPixelBufferRef buffer,size_t x,size_t y){
    auto type=CVPixelBufferGetPixelFormatType(buffer);Plane p{};plane(buffer,0,p);auto pixel=p.bytes+y*p.row+x*p.pixel;
    if(yuv(type)){
        Plane u{},v{};plane(buffer,1,u);plane(buffer,2,v);size_t cy=type==kCVPixelFormatType_422YpCbCr10BiPlanarFullRange?y:y/2;
        auto up=u.bytes+cy*u.row+(x/2)*u.pixel,vp=v.bytes+cy*v.row+(x/2)*v.pixel;
        double yy=*pixel,cb=*up,cr=*vp;
        if(tenBit(type)){yy=(*(uint16_t*)pixel>>6)*255.0/1023;cb=(*(uint16_t*)up>>6)*255.0/1023;cr=(*(uint16_t*)vp>>6)*255.0/1023;}
        return 0xff000000u|(byte(yy+1.402*(cr-128))<<16)|(byte(yy-.344136*(cb-128)-.714136*(cr-128))<<8)|byte(yy+1.772*(cb-128));
    }
    if(type==kCVPixelFormatType_16LE565){uint16_t value;memcpy(&value,pixel,2);return 0xff000000u|(((value>>11)*255/31)<<16)|((((value>>5)&63)*255/63)<<8)|((value&31)*255/31);}
    if(type==rgba1010102){uint32_t value;memcpy(&value,pixel,4);return ((value>>30)*85<<24)|((value&1023)*255/1023<<16)|(((value>>10)&1023)*255/1023<<8)|(((value>>20)&1023)*255/1023);}
    if(type==kCVPixelFormatType_64RGBAHalf){auto value=(__fp16*)pixel;return (uint32_t(byte(value[3]*255))<<24)|(byte(value[0]*255)<<16)|(byte(value[1]*255)<<8)|byte(value[2]*255);}
    if(type==kCVPixelFormatType_OneComponent8||type==kCVPixelFormatType_OneComponent16){uint32_t value=type==kCVPixelFormatType_OneComponent8?*pixel:(*(uint16_t*)pixel/257);return 0xff000000u|value*0x010101;}
    return ((type==kCVPixelFormatType_24RGB?255u:pixel[3])<<24)|(uint32_t(pixel[0])<<16)|(uint32_t(pixel[1])<<8)|pixel[2];
}
void store(CVPixelBufferRef buffer,size_t x,size_t y,uint32_t color,bool opaque){
    Plane p{};plane(buffer,0,p);auto pixel=p.bytes+y*p.row+x*p.pixel;auto type=CVPixelBufferGetPixelFormatType(buffer);
    unsigned r=(color>>16)&255,g=(color>>8)&255,b=color&255,a=opaque?255:color>>24;
    if(type==kCVPixelFormatType_16LE565){uint16_t value=(r*31/255<<11)|(g*63/255<<5)|(b*31/255);memcpy(pixel,&value,2);}
    else if(type==rgba1010102){uint32_t value=(r*1023/255)|(g*1023/255<<10)|(b*1023/255<<20)|(a*3/255<<30);memcpy(pixel,&value,4);}
    else if(type==kCVPixelFormatType_64RGBAHalf){auto value=(__fp16*)pixel;value[0]=r/255.0f;value[1]=g/255.0f;value[2]=b/255.0f;value[3]=a/255.0f;}
    else if(type==kCVPixelFormatType_OneComponent8)*pixel=byte(.299*r+.587*g+.114*b);
    else if(type==kCVPixelFormatType_OneComponent16){uint16_t value=byte(.299*r+.587*g+.114*b)*257;memcpy(pixel,&value,2);}
    else{pixel[0]=r;pixel[1]=g;pixel[2]=b;if(type!=kCVPixelFormatType_24RGB)pixel[3]=a;}
}
void storeYuv(CVPixelBufferRef buffer,const jint *colors){
    auto type=CVPixelBufferGetPixelFormatType(buffer);size_t width=CVPixelBufferGetWidth(buffer),height=CVPixelBufferGetHeight(buffer),stepY=type==kCVPixelFormatType_422YpCbCr10BiPlanarFullRange?1:2;
    Plane yy{},u{},v{};plane(buffer,0,yy);plane(buffer,1,u);plane(buffer,2,v);
    auto sample=[&](uint8_t *destination,double value){if(tenBit(type)){uint16_t result=uint16_t(std::clamp((int)std::lround(value*1023/255),0,1023))<<6;memcpy(destination,&result,2);}else *destination=byte(value);};
    for(size_t y=0;y<height;y++)for(size_t x=0;x<width;x++){uint32_t c=colors[y*width+x];sample(yy.bytes+y*yy.row+x*yy.pixel,.299*((c>>16)&255)+.587*((c>>8)&255)+.114*(c&255));}
    for(size_t y=0;y<height;y+=stepY)for(size_t x=0;x<width;x+=2){double r=0,g=0,b=0,n=0;
        for(size_t dy=0;dy<stepY&&y+dy<height;dy++)for(size_t dx=0;dx<2&&x+dx<width;dx++){uint32_t c=colors[(y+dy)*width+x+dx];r+=(c>>16)&255;g+=(c>>8)&255;b+=c&255;n++;}
        r/=n;g/=n;b/=n;sample(u.bytes+(y/stepY)*u.row+(x/2)*u.pixel,128-.168736*r-.331264*g+.5*b);sample(v.bytes+(y/stepY)*v.row+(x/2)*v.pixel,128+.5*r-.418688*g-.081312*b);
    }
}
}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_supportsStorage(JNIEnv*,jclass,jint format){return pixelFormat(format)!=0;}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_createStorage(JNIEnv *env,jclass,jint width,jint height,jint format){@autoreleasepool{
    registerFormats();auto type=pixelFormat(format);if(!type){error(env,"Hardware buffer format is unsupported");return 0;}
    NSDictionary *properties=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    CVPixelBufferRef buffer=nullptr;auto result=CVPixelBufferCreate(kCFAllocatorDefault,width,height,type,(__bridge CFDictionaryRef)properties,&buffer);
    if(result!=kCVReturnSuccess||!buffer){char message[96];snprintf(message,sizeof(message),"Hardware buffer allocation failed (CoreVideo %d)",result);error(env,message);return 0;}
    result=CVPixelBufferLockBaseAddress(buffer,0);if(result!=kCVReturnSuccess){CVPixelBufferRelease(buffer);error(env,"Hardware buffer storage cannot be mapped");return 0;}
    // Zero newly allocated storage, including row padding, before exposing it.
    if(CVPixelBufferIsPlanar(buffer)){for(size_t i=0;i<CVPixelBufferGetPlaneCount(buffer);i++)memset(CVPixelBufferGetBaseAddressOfPlane(buffer,i),0,CVPixelBufferGetBytesPerRowOfPlane(buffer,i)*CVPixelBufferGetHeightOfPlane(buffer,i));}
    else memset(CVPixelBufferGetBaseAddress(buffer),0,CVPixelBufferGetDataSize(buffer));
    void *owner=klj_hardwarebuffer_create(buffer);
    if(!owner){CVPixelBufferUnlockBaseAddress(buffer,0);error(env,"Hardware buffer ownership allocation failed");}
    CVPixelBufferRelease(buffer);return (jlong)(uintptr_t)owner;
}}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_releaseStorage(JNIEnv*,jclass,jlong handle){
    if(auto buffer=pixels(handle))CVPixelBufferUnlockBaseAddress(buffer,0);klj_hardwarebuffer_release((void*)(uintptr_t)handle);
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_surfaceStorage(JNIEnv*,jclass,jlong handle){auto buffer=pixels(handle);return buffer?(jlong)(uintptr_t)CVPixelBufferGetIOSurface(buffer):0;}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storageRowStride(JNIEnv*,jclass,jlong handle){Plane p{};return plane(pixels(handle),0,p)?(jint)p.row:0;}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storagePlaneCount(JNIEnv*,jclass,jlong handle){auto buffer=pixels(handle);return buffer?(yuv(CVPixelBufferGetPixelFormatType(buffer))?3:1):0;}
extern "C" JNIEXPORT jintArray JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storagePlaneLayout(JNIEnv *env,jclass,jlong handle,jint index){Plane p{};if(!plane(pixels(handle),index,p)){error(env,"Hardware buffer plane is unavailable");return nullptr;}jint values[]={(jint)p.row,(jint)p.pixel};auto result=env->NewIntArray(2);if(result)env->SetIntArrayRegion(result,0,2,values);return result;}
extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storagePlane(JNIEnv *env,jclass,jlong handle,jint index){Plane p{};if(!plane(pixels(handle),index,p)){error(env,"Hardware buffer plane is unavailable");return nullptr;}return env->NewDirectByteBuffer(p.bytes,p.size);}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storageSize(JNIEnv*,jclass,jlong handle){auto buffer=pixels(handle);return buffer?CVPixelBufferGetDataSize(buffer):0;}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostHardwareBuffer_storagePixels(JNIEnv *env,jclass,jlong handle,jintArray array,jboolean write,jboolean opaque){
    auto buffer=pixels(handle);if(!buffer){error(env,"Hardware buffer has closed");return;}
    size_t width=CVPixelBufferGetWidth(buffer),height=CVPixelBufferGetHeight(buffer);
    if(width*height>(size_t)env->GetArrayLength(array)){error(env,"Hardware buffer pixel array is too small");return;}
    auto colors=env->GetIntArrayElements(array,nullptr);if(!colors)return;
    if(write&&yuv(CVPixelBufferGetPixelFormatType(buffer)))storeYuv(buffer,colors);
    else for(size_t y=0;y<height;y++)for(size_t x=0;x<width;x++)if(write)store(buffer,x,y,colors[y*width+x],opaque);else colors[y*width+x]=argb(buffer,x,y)|(opaque?0xff000000u:0);
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
    id<MTLTexture> texture;
    if(CVPixelBufferGetPixelFormatType(buffer)==kCVPixelFormatType_32RGBA)texture=[device newTextureWithDescriptor:description iosurface:CVPixelBufferGetIOSurface(buffer) plane:0];
    else{texture=[device newTextureWithDescriptor:description];if(texture){std::vector<uint32_t> rgba(size_t(width)*height);for(int y=0;y<height;y++)for(int x=0;x<width;x++){auto c=argb(buffer,x,y);rgba[size_t(y)*width+x]=(c&0xff00ff00u)|((c>>16)&255)|((c&255)<<16);}[texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:size_t(width)*4];}}
    if(!texture){error(env,"Hardware buffer cannot be imported by Metal");return -22;}
    auto commands=[queue commandBuffer];commands.label=@"Android hardware buffer presentation";
    int result=klj_metal_project(env,presenter,nullptr,(__bridge void*)commands,(__bridge void*)texture);if(!result)[commands commit];
    return result==0?0:result==3?-11:-22;
}}
