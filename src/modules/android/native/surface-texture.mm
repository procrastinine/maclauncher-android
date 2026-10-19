#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include "angle-metal.hpp"
#include "surface-format.hpp"
#include "surface-texture.hpp"
#include "video-pixels.hpp"
#include "native-clock/clock-domain.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <ctime>
#include <cstdint>
#include <cstdio>

extern "C" void *klj_graphics_symbol(const char*);
namespace {
template<class T>T gl(const char *name){return reinterpret_cast<T>(klj_graphics_symbol(name));}
using Display=void*;using Context=void*;using Image=void*;using Sync=void*;
JavaVM *vm=nullptr;
struct Attached {JNIEnv*env=nullptr;bool owned=false;Attached(){if(vm&&vm->GetEnv((void**)&env,JNI_VERSION_1_8)==JNI_EDETACHED)owned=vm->AttachCurrentThreadAsDaemon((void**)&env,nullptr)==JNI_OK;}~Attached(){if(owned)vm->DetachCurrentThread();}};
void failure(JNIEnv*env,const char*message){if(env->ExceptionCheck())return;jclass type=env->FindClass("java/lang/IllegalStateException");if(type){env->ThrowNew(type,message);env->DeleteLocalRef(type);}}
jclass hostClass=nullptr,handleClass=nullptr,textureClass=nullptr;
jmethodID makeBuffer,bufferLease,resizeBuffer,retainHandle,releaseHandle,postEvent;
jfieldID nativePointer;
std::once_flag initialized;
void initialize(JNIEnv*env,jclass type){
 env->GetJavaVM(&vm);textureClass=(jclass)env->NewGlobalRef(type);
 jclass local=env->FindClass("maclauncher/android/framework/HostSurfaceTexture");hostClass=(jclass)env->NewGlobalRef(local);env->DeleteLocalRef(local);
 local=env->FindClass("maclauncher/android/framework/HostNativeHandle");handleClass=(jclass)env->NewGlobalRef(local);env->DeleteLocalRef(local);
 makeBuffer=env->GetStaticMethodID(hostClass,"createBuffer","()J");bufferLease=env->GetStaticMethodID(hostClass,"bufferLease","(J)J");resizeBuffer=env->GetStaticMethodID(hostClass,"resizeBuffer","(JII)V");
 retainHandle=env->GetStaticMethodID(handleClass,"retain","(J)J");releaseHandle=env->GetStaticMethodID(handleClass,"release","(J)V");
 nativePointer=env->GetFieldID(type,"mSurfaceTexture","J");postEvent=env->GetStaticMethodID(type,"postEventFromNative","(Ljava/lang/ref/WeakReference;)V");
}
struct Frame {
 Display display=nullptr;Image image=nullptr;Sync fence=nullptr,producedFence=nullptr;
 id<MTLTexture> metal=nil;
 int width=0,height=0;int64_t timestamp=0;bool canvas=false;
 std::vector<unsigned char> pixels;
 CVPixelBufferRef video=nullptr;void(*completed)(void*,int64_t)=nullptr;void*completion=nullptr;
 ~Frame(){if(video)CVPixelBufferRelease(video);if(completed)completed(completion,0);if(producedFence)gl<unsigned(*)(Display,Sync)>("eglDestroySync")(display,producedFence);if(fence)gl<unsigned(*)(Display,Sync)>("eglDestroySyncKHR")(display,fence);if(image)gl<unsigned(*)(Display,Image)>("eglDestroyImageKHR")(display,image);}
};
struct Texture {
 std::mutex mutex;
 uint64_t lease=0;jlong buffer=0;jobject weak=nullptr;
 bool released=false,detached=false,single=false;
 unsigned name=0;Context context=nullptr;
 std::shared_ptr<Frame> latest,acquired;
 id<MTLDevice> device=nil;id<MTLCommandQueue> queue=nil;id<MTLSharedEvent> produced=nil,copied=nil;uint64_t serial=0;
 ~Texture(){latest.reset();acquired.reset();Attached thread;if(thread.env){auto env=thread.env;jthrowable pending=env->ExceptionOccurred();if(pending)env->ExceptionClear();if(buffer)env->CallStaticVoidMethod(handleClass,releaseHandle,buffer);if(weak)env->DeleteGlobalRef(weak);if(pending){if(env->ExceptionCheck())env->ExceptionClear();env->Throw(pending);env->DeleteLocalRef(pending);}}}
};
std::mutex ownersMutex;
std::unordered_map<Texture*,std::shared_ptr<Texture>> owners;
std::unordered_map<uint64_t,std::weak_ptr<Texture>> producers;
std::shared_ptr<Texture> state(JNIEnv*env,jobject object){
 if(!nativePointer){failure(env,"SurfaceTexture is not initialized");return {};}
 auto pointer=(Texture*)(uintptr_t)env->GetLongField(object,nativePointer);std::lock_guard lock(ownersMutex);auto entry=owners.find(pointer);
 if(entry==owners.end()){failure(env,"SurfaceTexture is released");return {};}return entry->second;
}
std::shared_ptr<Texture> producer(uint64_t lease){std::lock_guard lock(ownersMutex);auto entry=producers.find(lease);return entry==producers.end()?nullptr:entry->second.lock();}
bool alive(JNIEnv*env,const Texture&texture){if(texture.released){failure(env,"SurfaceTexture is released");return false;}return true;}
void notify(JNIEnv*env,const std::shared_ptr<Texture>&texture){env->CallStaticVoidMethod(textureClass,postEvent,texture->weak);}
Context current(){return gl<Context(*)()>("eglGetCurrentContext")();}
Display display(){return gl<Display(*)()>("eglGetCurrentDisplay")();}
/** ANGLE Metal imports native Metal images. Reuse the presenter's shared-event
 * protocol for producer -> snapshot -> consumer ordering without CPU waits. */
bool image(JNIEnv*env,Texture&owner,Frame&frame,void*window){@autoreleasepool{try{
 frame.display=display();if(!current()||!frame.display){failure(env,"SurfaceTexture requires a current EGL context");return false;}
 auto device=klj_angle::device(frame.display);if(owner.device&&owner.device!=device){failure(env,"SurfaceTexture graphics device changed");return false;}
 if(!owner.device){owner.device=device;owner.queue=[device newCommandQueue];owner.produced=[device newSharedEvent];owner.copied=[device newSharedEvent];}
 if(!owner.queue||!owner.produced||!owner.copied){failure(env,"SurfaceTexture Metal synchronization is unavailable");return false;}
 auto storage=window?(IOSurfaceRef)klj_surface_copy_storage(window):nullptr;
 if(window&&!storage){failure(env,"SurfaceTexture producer storage is unavailable");return false;}
 std::unique_ptr<const __IOSurface,decltype(&CFRelease)> storageOwner(storage,&CFRelease);
 auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:storage?klj_surface_format::metal(storage):MTLPixelFormatRGBA8Unorm width:frame.width height:frame.height mipmapped:NO];descriptor.storageMode=window?MTLStorageModePrivate:MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 frame.metal=[device newTextureWithDescriptor:descriptor];if(!frame.metal){failure(env,"SurfaceTexture image allocation failed");return false;}
 if(window){
  descriptor.storageMode=MTLStorageModeShared;auto source=[device newTextureWithDescriptor:descriptor iosurface:storage plane:0];if(!source){failure(env,"SurfaceTexture producer image cannot be imported");return false;}
  uint64_t serial=++owner.serial;frame.producedFence=klj_angle::sharedSync(frame.display,owner.produced,serial,false);frame.fence=klj_angle::sharedSync(frame.display,owner.copied,serial,true);
  if(!frame.producedFence||!frame.fence){failure(env,"ANGLE could not fence the SurfaceTexture image");return false;}
  gl<void(*)()>("glFlush")();auto commands=[owner.queue commandBuffer];commands.label=@"Android SurfaceTexture image";
  [commands encodeWaitForEvent:owner.produced value:serial];auto blit=[commands blitCommandEncoder];[blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(frame.width,frame.height,1) toTexture:frame.metal destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];[commands encodeSignalEvent:owner.copied value:serial];[commands commit];
  if(!gl<unsigned(*)(Display,Sync,int)>("eglWaitSync")(frame.display,frame.fence,0)){failure(env,"ANGLE could not protect SurfaceTexture producer reuse");return false;}
 }else if(frame.video){
  auto commands=[owner.queue commandBuffer];commands.label=@"Android decoded SurfaceTexture video";
  frame.metal=klj_video_texture(frame.video,device,commands);if(!frame.metal){failure(env,"Decoded video image allocation failed");return false;}
  uint64_t serial=++owner.serial;frame.fence=klj_angle::sharedSync(frame.display,owner.copied,serial,true);
  if(!frame.fence){failure(env,"ANGLE could not fence decoded video");return false;}
  [commands encodeSignalEvent:owner.copied value:serial];
  auto callback=frame.completed;auto context=frame.completion;frame.completed=nullptr;frame.completion=nullptr;
  CVPixelBufferRef retained=CVPixelBufferRetain(frame.video);
  [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer){if(callback){timespec now;klj_clock_read(1,&now);callback(context,buffer.status==MTLCommandBufferStatusCompleted?int64_t(now.tv_sec)*1000000000+now.tv_nsec:0);}CVPixelBufferRelease(retained);}];
  [commands commit];
 }else [frame.metal replaceRegion:MTLRegionMake2D(0,0,frame.width,frame.height) mipmapLevel:0 withBytes:frame.pixels.data() bytesPerRow:frame.width*4];
 const int attributes[]={0x3038};frame.image=gl<Image(*)(Display,Context,unsigned,void*,const int*)>("eglCreateImageKHR")(frame.display,nullptr,0x34A7,(__bridge void*)frame.metal,attributes);
 if(!frame.image){char message[128];std::snprintf(message,sizeof message,"ANGLE could not import the SurfaceTexture Metal image (EGL 0x%x)",gl<unsigned(*)()>("eglGetError")());failure(env,message);return false;}
 frame.pixels.clear();frame.pixels.shrink_to_fit();return true;
}catch(const std::exception&error){failure(env,error.what());return false;}}}

}

extern "C" JNIEXPORT void JNICALL Java_android_graphics_SurfaceTexture_nativeInit(JNIEnv*env,jobject object,jboolean detached,jint name,jboolean single,jobject weak){
 jclass type=env->GetObjectClass(object);std::call_once(initialized,initialize,env,type);env->DeleteLocalRef(type);if(env->ExceptionCheck())return;
 auto texture=std::make_shared<Texture>();texture->detached=detached;texture->name=(unsigned)name;texture->single=single;texture->context=detached?nullptr:current();
 texture->weak=env->NewGlobalRef(weak);texture->buffer=env->CallStaticLongMethod(hostClass,makeBuffer);if(env->ExceptionCheck())return;
 texture->lease=env->CallStaticLongMethod(hostClass,bufferLease,texture->buffer);if(env->ExceptionCheck())return;
 if(!klj_surface_texture_owner(texture->lease)){failure(env,"SurfaceTexture producer is unavailable");return;}
 {std::lock_guard lock(ownersMutex);owners.emplace(texture.get(),texture);producers.emplace(texture->lease,texture);}
 env->SetLongField(object,nativePointer,(jlong)(uintptr_t)texture.get());
}
extern "C" JNIEXPORT void JNICALL Java_android_graphics_SurfaceTexture_nativeRelease(JNIEnv*env,jobject object){auto texture=state(env,object);if(!texture)return;std::lock_guard lock(texture->mutex);texture->released=true;texture->latest.reset();texture->acquired.reset();if(texture->buffer){jlong buffer=texture->buffer;texture->buffer=0;env->CallStaticVoidMethod(handleClass,releaseHandle,buffer);}}
extern "C" JNIEXPORT void JNICALL Java_android_graphics_SurfaceTexture_nativeFinalize(JNIEnv*env,jobject object){
 if(!nativePointer)return;auto pointer=(Texture*)(uintptr_t)env->GetLongField(object,nativePointer);std::shared_ptr<Texture> texture;
 {std::lock_guard lock(ownersMutex);auto entry=owners.find(pointer);if(entry==owners.end())return;texture=entry->second;producers.erase(texture->lease);owners.erase(entry);}env->SetLongField(object,nativePointer,0);
}
extern "C" JNIEXPORT jboolean JNICALL Java_android_graphics_SurfaceTexture_nativeIsReleased(JNIEnv*env,jobject object){auto texture=state(env,object);if(!texture)return true;std::lock_guard lock(texture->mutex);return texture->released;}
extern "C" JNIEXPORT void JNICALL Java_android_graphics_SurfaceTexture_nativeSetDefaultBufferSize(JNIEnv*env,jobject object,jint width,jint height){auto texture=state(env,object);if(!texture)return;std::lock_guard lock(texture->mutex);if(alive(env,*texture))env->CallStaticVoidMethod(hostClass,resizeBuffer,texture->buffer,width,height);}
extern "C" JNIEXPORT void JNICALL Java_android_graphics_SurfaceTexture_nativeGetTransformMatrix(JNIEnv*env,jobject object,jfloatArray output){
 auto texture=state(env,object);if(!texture)return;std::lock_guard lock(texture->mutex);if(!alive(env,*texture))return;
 // GL producers originate at the lower-left; Canvas pixels originate at top-left.
 bool flip=!texture->acquired||!texture->acquired->canvas;const float matrix[]={1,0,0,0,0,flip?-1.f:1.f,0,0,0,0,1,0,0,flip?1.f:0.f,0,1};env->SetFloatArrayRegion(output,0,16,matrix);
}
extern "C" JNIEXPORT jlong JNICALL Java_android_graphics_SurfaceTexture_nativeGetTimestamp(JNIEnv*env,jobject object){auto texture=state(env,object);if(!texture)return 0;std::lock_guard lock(texture->mutex);return texture->acquired?texture->acquired->timestamp:0;}
extern "C" JNIEXPORT jint JNICALL Java_android_graphics_SurfaceTexture_nativeGetDataSpace(JNIEnv*env,jobject object){auto texture=state(env,object);if(!texture)return 0;std::lock_guard lock(texture->mutex);return texture->acquired&&texture->acquired->canvas?0x8810000:0;}
extern "C" JNIEXPORT void JNICALL Java_android_graphics_SurfaceTexture_nativeUpdateTexImage(JNIEnv*env,jobject object){
 auto texture=state(env,object);if(!texture)return;std::lock_guard lock(texture->mutex);if(!alive(env,*texture))return;
 Context context=current();if(texture->detached||!context||(texture->context&&texture->context!=context)){failure(env,"SurfaceTexture is not attached to the current EGL context");return;}texture->context=context;
 auto frame=texture->latest;if(!frame||frame==texture->acquired)return;
 const char*extensions=(const char*)gl<const unsigned char*(*)(unsigned)>("glGetString")(0x1F03);
 constexpr char externalName[]="GL_OES_EGL_image_external";const char*external=extensions?std::strstr(extensions,externalName):nullptr;
 if(!external||(external[sizeof(externalName)-1]!=' '&&external[sizeof(externalName)-1]!='\0')){static std::once_flag diagnostic;std::call_once(diagnostic,[]{std::fprintf(stderr,"[android-graphics] SurfaceTexture sampling requires GL_OES_EGL_image_external; the active graphics provider does not expose it\n");});return;}
 if(!frame->image&&!image(env,*texture,*frame,nullptr))return;
 if(frame->display!=display()){failure(env,"SurfaceTexture producer and consumer use different EGL displays");return;}
 if(frame->fence&&!gl<unsigned(*)(Display,Sync,int)>("eglWaitSyncKHR")(frame->display,frame->fence,0)){failure(env,"ANGLE could not wait for the SurfaceTexture image");return;}
 gl<void(*)(unsigned,unsigned)>("glBindTexture")(0x8D65,texture->name);gl<void(*)(unsigned,void*)>("glEGLImageTargetTexture2DOES")(0x8D65,frame->image);texture->acquired=std::move(frame);
}
extern "C" JNIEXPORT void JNICALL Java_android_graphics_SurfaceTexture_nativeReleaseTexImage(JNIEnv*env,jobject object){auto texture=state(env,object);if(!texture)return;std::lock_guard lock(texture->mutex);if(alive(env,*texture))texture->acquired.reset();}
extern "C" JNIEXPORT jint JNICALL Java_android_graphics_SurfaceTexture_nativeDetachFromGLContext(JNIEnv*env,jobject object){
 auto texture=state(env,object);if(!texture)return -19;std::lock_guard lock(texture->mutex);if(texture->released||texture->detached)return -38;
 Context context=current();if(!context||(texture->context&&context!=texture->context))return -22;
 gl<void(*)(int,const unsigned*)>("glDeleteTextures")(1,&texture->name);texture->name=0;texture->context=nullptr;texture->detached=true;texture->acquired.reset();return 0;
}
extern "C" JNIEXPORT jint JNICALL Java_android_graphics_SurfaceTexture_nativeAttachToGLContext(JNIEnv*env,jobject object,jint name){
 auto texture=state(env,object);if(!texture)return -19;std::lock_guard lock(texture->mutex);if(texture->released||!texture->detached)return -38;
 Context context=current();if(!context||name<=0)return -22;texture->name=(unsigned)name;texture->context=context;texture->detached=false;return 0;
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostSurfaceTexture_acquireBuffer(JNIEnv*env,jclass,jobject object){auto texture=state(env,object);if(!texture)return 0;std::lock_guard lock(texture->mutex);return alive(env,*texture)?env->CallStaticLongMethod(handleClass,retainHandle,texture->buffer):0;}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostSurfaceTexture_retireOwners(JNIEnv*,jclass){std::unordered_map<Texture*,std::shared_ptr<Texture>> retired;{std::lock_guard lock(ownersMutex);producers.clear();retired.swap(owners);}for(auto&entry:retired){std::lock_guard lock(entry.second->mutex);entry.second->released=true;entry.second->latest.reset();entry.second->acquired.reset();}}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_framework_HostSurfaceTexture_submitCanvas(JNIEnv*env,jclass,jlong lease,jintArray pixels,jint width,jint height,jlong timestamp){
 auto texture=producer(lease);if(!texture){failure(env,"SurfaceTexture consumer is unavailable");return false;}
 auto frame=std::make_shared<Frame>();frame->width=width;frame->height=height;frame->timestamp=timestamp;frame->canvas=true;
 size_t count=size_t(width)*height;if(width<=0||height<=0||count>67108864||env->GetArrayLength(pixels)<(jsize)count){failure(env,"SurfaceTexture Canvas dimensions are invalid");return false;}
 frame->pixels.resize(count*4);std::vector<jint> row(width);
 for(int y=0;y<height;y++){env->GetIntArrayRegion(pixels,y*width,width,row.data());if(env->ExceptionCheck())return false;for(int x=0;x<width;x++){uint32_t argb=(uint32_t)row[x];auto*rgba=frame->pixels.data()+(size_t(y)*width+x)*4;rgba[0]=argb>>16;rgba[1]=argb>>8;rgba[2]=argb;rgba[3]=argb>>24;}}
 {std::lock_guard lock(texture->mutex);if(!alive(env,*texture))return false;if(texture->single&&texture->acquired)return false;texture->latest=std::move(frame);}notify(env,texture);return !env->ExceptionCheck();
}
extern "C" unsigned klj_texture_publish(JNIEnv*env,void*display,void*window){
 auto texture=producer(klj_surface_lease(window));if(!texture)return 0;
 auto frame=std::make_shared<Frame>();frame->display=display;
 int width=0,height=0;void*surface=gl<void*(*)(int)>("eglGetCurrentSurface")(0x3059);auto query=gl<unsigned(*)(Display,void*,int,int*)>("eglQuerySurface");if(!query(display,surface,0x3057,&width)||!query(display,surface,0x3056,&height))return 0;
 frame->width=width;frame->height=height;timespec now;klj_clock_read(1,&now);frame->timestamp=int64_t(now.tv_sec)*1000000000+now.tv_nsec;
 {std::lock_guard lock(texture->mutex);if(!alive(env,*texture)||texture->single&&texture->acquired)return 0;if(!image(env,*texture,*frame,window))return 0;texture->latest=std::move(frame);}notify(env,texture);return env->ExceptionCheck()?0:1;
}
extern "C" bool klj_texture_publish_video(JNIEnv*env,uint64_t lease,CVPixelBufferRef pixels,int64_t timestamp,void(*completed)(void*,int64_t),void*context){
 auto texture=producer(lease);if(!texture)return false;
 auto frame=std::make_shared<Frame>();frame->width=(int)CVPixelBufferGetWidth(pixels);frame->height=(int)CVPixelBufferGetHeight(pixels);frame->timestamp=timestamp;frame->canvas=true;frame->video=CVPixelBufferRetain(pixels);
 {std::lock_guard lock(texture->mutex);if(texture->released||(texture->single&&texture->acquired))return false;frame->completed=completed;frame->completion=context;texture->latest=std::move(frame);}
 notify(env,texture);
 // The retained frame now owns completion even if the Java notification fails.
 return true;
}
