#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include "composition-target.h"
#include "angle-metal.hpp"
#include "surface-format.hpp"
#include <memory>
#include <mutex>
#include <unistd.h>

namespace {
struct Environment {
 JavaVM *vm;JNIEnv *env=nullptr;bool attached=false;
 explicit Environment(JavaVM *value):vm(value){if(vm->GetEnv((void**)&env,JNI_VERSION_1_8)==JNI_EDETACHED)attached=vm->AttachCurrentThreadAsDaemon((void**)&env,nullptr)==JNI_OK;}
 ~Environment(){if(attached)vm->DetachCurrentThread();}
};
void failure(JNIEnv*env,const char*message){if(env->ExceptionCheck())return;auto type=env->FindClass("java/lang/IllegalStateException");if(type){env->ThrowNew(type,message);env->DeleteLocalRef(type);}}
struct Target {
 JavaVM *vm=nullptr;jobject producer=nullptr;
 jmethodID dequeue,reserve,pixels,width,height,stride,queue,cancel;
 id<MTLDevice> device=nil;id<MTLCommandQueue> commands=nil;id<MTLSharedEvent> produced=nil,copied=nil;
 uint64_t sequence=0;std::mutex mutex;
 ~Target(){Environment thread(vm);if(thread.env&&producer)thread.env->DeleteGlobalRef(producer);}
};
using Owner=std::shared_ptr<Target>;
template<class T>T gl(const char *name){return reinterpret_cast<T>(klj_graphics_symbol(name));}
}
extern "C" void *klj_composition_create(JNIEnv*env,jobject producer){
 auto owner=std::make_shared<Target>();env->GetJavaVM(&owner->vm);owner->producer=env->NewGlobalRef(producer);
 auto type=env->GetObjectClass(producer);owner->dequeue=env->GetMethodID(type,"dequeueBlocking","()Lmaclauncher/android/bridge/NativeComposition$Layer$Buffer;");owner->reserve=env->GetMethodID(type,"reserveFrame","()J");env->DeleteLocalRef(type);
 type=env->FindClass("maclauncher/android/bridge/NativeComposition$Layer$Buffer");if(!type)return nullptr;
 owner->pixels=env->GetMethodID(type,"pixels","()Ljava/nio/ByteBuffer;");owner->width=env->GetMethodID(type,"width","()I");owner->height=env->GetMethodID(type,"height","()I");owner->stride=env->GetMethodID(type,"stride","()I");owner->queue=env->GetMethodID(type,"queue","(Landroid/os/ParcelFileDescriptor;J)V");owner->cancel=env->GetMethodID(type,"close","()V");env->DeleteLocalRef(type);
 if(env->ExceptionCheck()||!owner->producer)return nullptr;return new Owner(std::move(owner));
}
extern "C" void klj_composition_release(void*pointer){delete static_cast<Owner*>(pointer);}

// The IOSurface is ANGLE's producer attachment, never a second display or
// compositor. Its GPU result enters the same original gralloc dequeue as
// Canvas. Only actual GPU completion queues that buffer to SurfaceFlinger.
extern "C" int klj_composition_present(JNIEnv*env,void*pointer,void*display,void*storage,uint64_t frame){@autoreleasepool{
 auto owner=*static_cast<Owner*>(pointer);std::lock_guard lock(owner->mutex);jobject acquired=nullptr,mapping=nullptr,retained=nullptr;void *producedSync=nullptr,*copiedSync=nullptr;
 auto abandon=[&]{if(acquired){env->CallVoidMethod(acquired,owner->cancel);env->DeleteLocalRef(acquired);}if(mapping)env->DeleteLocalRef(mapping);if(retained)env->DeleteGlobalRef(retained);if(producedSync)gl<unsigned(*)(void*,void*)>("eglDestroySync")(display,producedSync);if(copiedSync)gl<unsigned(*)(void*,void*)>("eglDestroySync")(display,copiedSync);return 0;};
 try{
  frame=(uint64_t)env->CallLongMethod(owner->producer,owner->reserve);if(env->ExceptionCheck())return abandon();
  acquired=env->CallObjectMethod(owner->producer,owner->dequeue);if(env->ExceptionCheck()||!acquired)return abandon();
  mapping=env->CallObjectMethod(acquired,owner->pixels);if(env->ExceptionCheck()||!mapping)return abandon();
  auto address=env->GetDirectBufferAddress(mapping);auto length=env->GetDirectBufferCapacity(mapping);int width=env->CallIntMethod(acquired,owner->width),height=env->CallIntMethod(acquired,owner->height),stride=env->CallIntMethod(acquired,owner->stride);
  if(env->ExceptionCheck())return abandon();
  if(!address||length<=0||(uintptr_t)address%getpagesize()||length%getpagesize()||width<1||height<1||stride<width||(uint64_t)stride*height*4>(uint64_t)length){failure(env,"Native composition mapping is invalid");return abandon();}
  auto source=static_cast<IOSurfaceRef>(storage);if(IOSurfaceGetWidth(source)!=(size_t)width||IOSurfaceGetHeight(source)!=(size_t)height||klj_surface_format::metal(source)!=MTLPixelFormatRGBA8Unorm){failure(env,"Native composition producer geometry does not match its buffer");return abandon();}
  auto device=klj_angle::device(display);if(owner->device&&owner->device!=device){failure(env,"Native composition graphics device changed");return abandon();}
  if(!owner->device){owner->device=device;owner->commands=[device newCommandQueue];owner->produced=[device newSharedEvent];owner->copied=[device newSharedEvent];}
  auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
  id<MTLBuffer> memory=[device newBufferWithBytesNoCopy:address length:(NSUInteger)length options:MTLResourceStorageModeShared deallocator:nil];
  id<MTLTexture> destination=[memory newTextureWithDescriptor:descriptor offset:0 bytesPerRow:(NSUInteger)stride*4];
  id<MTLTexture> input=[device newTextureWithDescriptor:descriptor iosurface:source plane:0];
  if(!memory||!destination||!input||!owner->commands||!owner->produced||!owner->copied){failure(env,"Metal cannot borrow the original composition buffers");return abandon();}
  uint64_t sequence=++owner->sequence;producedSync=klj_angle::sharedSync(display,owner->produced,sequence,false);copiedSync=klj_angle::sharedSync(display,owner->copied,sequence,true);
  if(!producedSync||!copiedSync){failure(env,"ANGLE cannot synchronize the native composition producer");return abandon();}
  gl<void(*)()>("glFlush")();id<MTLCommandBuffer> commands=[owner->commands commandBuffer];commands.label=@"Android independent composition layer";
  [commands encodeWaitForEvent:owner->produced value:sequence];auto blit=[commands blitCommandEncoder];[blit copyFromTexture:input sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1) toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];[commands encodeSignalEvent:owner->copied value:sequence];
  // Queue the source protection before committing the copy. No following draw
  // can overwrite ANGLE's attachment while this independent layer reads it.
  retained=env->NewGlobalRef(acquired);if(!retained)return abandon();
  if(!gl<unsigned(*)(void*,void*,int)>("eglWaitSync")(display,copiedSync,0)){failure(env,"ANGLE cannot protect the composition attachment");return abandon();}
  jobject completedBuffer=retained;retained=nullptr;
  [commands addCompletedHandler:^(id<MTLCommandBuffer> done){Environment thread(owner->vm);if(!thread.env)return;auto callback=thread.env;
   if(done.status==MTLCommandBufferStatusCompleted)callback->CallVoidMethod(completedBuffer,owner->queue,nullptr,(jlong)frame);else callback->CallVoidMethod(completedBuffer,owner->cancel);
   if(callback->ExceptionCheck()){callback->ExceptionDescribe();callback->ExceptionClear();}callback->DeleteGlobalRef(completedBuffer);
  }];
  [commands commit];env->DeleteLocalRef(acquired);acquired=nullptr;env->DeleteLocalRef(mapping);mapping=nullptr;
  gl<unsigned(*)(void*,void*)>("eglDestroySync")(display,producedSync);gl<unsigned(*)(void*,void*)>("eglDestroySync")(display,copiedSync);return 1;
 }catch(const std::exception&error){failure(env,error.what());return abandon();}
}}
