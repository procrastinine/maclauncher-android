// Reuse Klepton projection composition in the existing WindowSession. The
// compositor writes a bounded offscreen image; the shared presenter owns all
// effects, scaling, frame generation and the sole drawable presentation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "xr-presentation.h"
#include "xr-compositor.h"
#include "xr-context.h"
#include "presentation.h"
#include "xr-input.hpp"
#include <array>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {
struct Lease;
struct Slot {id<MTLTexture> texture,view;bool busy=false;size_t bytes=0;std::weak_ptr<Lease> lease;};
struct Pool {std::mutex mutex;std::array<Slot,3> slots;NSString *error=nil;};
struct Lease {
 std::shared_ptr<Pool> pool;size_t index;
 void completed(id<MTLCommandBuffer> commands){std::lock_guard lock(pool->mutex);if(commands.status==MTLCommandBufferStatusError&&!pool->error)pool->error=commands.error.localizedDescription?:@"Projection command buffer failed";}
 ~Lease(){std::lock_guard lock(pool->mutex);pool->slots[index].busy=false;}
};
struct Owner {
 JNIEnv *env;void *presenter;CAMetalLayer *layer;
 std::shared_ptr<Pool> pool=std::make_shared<Pool>();
 id<MTLSharedEvent> consumed;id<MTLCommandBuffer> last;
 bool started=false,retired=false,drained=false,failed=false;
 KlDesktopXrInput controls;SDL_Window *window=nullptr;
};
Owner *owner=nullptr; // The existing WindowSession is the sole main-thread owner.
void fail(JNIEnv *env,const char *text){if(env->ExceptionCheck())return;auto type=env->FindClass("java/lang/IllegalStateException");if(type)env->ThrowNew(type,text);}
}
extern "C" void *klj_xr_attach(JNIEnv *env,void *presenter,void *layer){
 if(owner||!presenter||!layer||![NSThread isMainThread]){fail(env,"Android projection window ownership is invalid");return nullptr;}
 owner=new Owner;owner->env=env;owner->presenter=presenter;owner->layer=(__bridge CAMetalLayer*)layer;return owner;
}
extern "C" int klj_xr_pump(JNIEnv *env,void *value,int width,int height){@autoreleasepool{
 auto p=static_cast<Owner*>(value);if(!p||p!=owner||p->retired||env->ExceptionCheck())return 0;
 {std::lock_guard lock(p->pool->mutex);if(p->pool->error){p->failed=true;fail(env,p->pool->error.UTF8String);return 0;}}
 if(klj_glfb_consumer_failed()){fail(env,"Android projection synchronization failed");return 0;}
 if(!p->started)p->started=klj_xr_compositor_start((__bridge void*)p->layer)!=0;
 if(p->started)klj_xr_compositor_present(width,height);
 return !p->failed&&!env->ExceptionCheck();
}}
extern "C" id<MTLTexture> klj_xr_output(id<MTLCommandBuffer> commands,int width,int height){
 auto p=owner;if(!p||p->retired)return nil;
 if(width<1||height<1||width>8192||height>8192){p->failed=true;fail(p->env,"Projection output dimensions are unsupported");return nil;}
 auto pool=p->pool;std::lock_guard lock(pool->mutex);
 size_t total=0,index=pool->slots.size();
 for(size_t i=0;i<pool->slots.size();i++){
  auto &slot=pool->slots[i];
  if(!slot.busy&&slot.texture&&(slot.texture.width!=(NSUInteger)width||slot.texture.height!=(NSUInteger)height||slot.texture.device!=commands.device)){slot.texture=nil;slot.view=nil;slot.bytes=0;}
  total+=slot.bytes;if(!slot.busy&&index==pool->slots.size())index=i;
 }
 if(index==pool->slots.size())return nil;
 auto &slot=pool->slots[index];
 if(!slot.texture){
  auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
  descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
  auto allocation=[commands.device heapTextureSizeAndAlignWithDescriptor:descriptor];
  constexpr size_t budget=192ull*1024*1024;
  if(allocation.size>budget){p->failed=true;fail(p->env,"Projection output exceeds the graphics memory limit");return nil;}
  if(total>budget-allocation.size)return nil;
  slot.texture=[commands.device newTextureWithDescriptor:descriptor];
  slot.view=[slot.texture newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
  if(!slot.texture||!slot.view){slot.texture=nil;slot.view=nil;p->failed=true;fail(p->env,"Projection output allocation failed");return nil;}
  slot.bytes=allocation.size;
 }
 slot.busy=true;
 auto lease=std::make_shared<Lease>();lease->pool=pool;lease->index=index;
 slot.lease=lease;
 [commands addCompletedHandler:^(id<MTLCommandBuffer> commands){lease->completed(commands);}];
 return slot.view;
}
extern "C" id<MTLCommandQueue> klj_xr_queue(id<MTLDevice> device){auto p=owner;if(!p)return nil;return (__bridge id<MTLCommandQueue>)klj_metal_projection_queue(p->env,p->presenter,(__bridge void*)device);}
extern "C" void klj_xr_fences(id<MTLSharedEvent> produced,int gl){
 auto p=owner;if(!p)return;
 if(gl){p->consumed=[produced.device newSharedEvent];if(!p->consumed){p->failed=true;fail(p->env,"Projection reuse event is unavailable");return;}klj_glfb_consumer((__bridge void*)p->consumed);}
}
extern "C" void klj_xr_submit(id<MTLCommandBuffer> commands,id<MTLTexture> target,uint64_t value){
 auto p=owner;if(!p)return;
 id<MTLTexture> source=nil;std::shared_ptr<Lease> lease;
 {std::lock_guard lock(p->pool->mutex);for(auto &slot:p->pool->slots)if(slot.view==target){source=slot.texture;lease=slot.lease.lock();break;}}
 if(!source||!lease){p->failed=true;fail(p->env,"Projection source ownership is invalid");}
 else if(klj_metal_project_retained(p->env,p->presenter,klj_glfb_display(),(__bridge void*)commands,(__bridge void*)source,
     new std::shared_ptr<Lease>(std::move(lease)),[](void*value){delete static_cast<std::shared_ptr<Lease>*>(value);})==4)p->failed=true;
 // Even a busy shared presenter has finished reading this source. Release the
 // original producer through its GPU queue, never through a CPU frame wait.
 klj_xr_discard(commands,value);
}
extern "C" void klj_xr_discard(id<MTLCommandBuffer> commands,uint64_t value){auto p=owner;if(!p)return;if(p->consumed)[commands encodeSignalEvent:p->consumed value:value];p->last=commands;}
extern "C" void klj_xr_begin(id<MTLCommandBuffer> commands,uint64_t value){
 // The guest may queue several frames before this owner iteration. Release
 // skipped frames BEFORE waiting for the selected producer fence; otherwise
 // ANGLE could wait on frame N while our queue waits on frame N+1.
 auto p=owner;if(p&&p->consumed&&value)[commands encodeSignalEvent:p->consumed value:value-1];
}
extern "C" int klj_xr_retire(void *value){
 auto p=static_cast<Owner*>(value);if(!p||p!=owner)return 1;if(p->retired)return p->drained;p->retired=true;
 uint64_t pending=klj_glfb_retire_consumer();
 if(p->window)p->controls.release(p->window);
 // Teardown only: finish our last reads before unblocking a guest joining its
 // renderer. The event objects remain retained until that guest is stopped.
 if(p->last&&p->last.status<MTLCommandBufferStatusCompleted){
  dispatch_semaphore_t done=dispatch_semaphore_create(0);[p->last addCompletedHandler:^(id<MTLCommandBuffer>){dispatch_semaphore_signal(done);}];
  if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC))){p->failed=true;fail(p->env,"Projection cleanup timed out waiting for its producer");return 0;}
 }
 if(p->consumed&&p->consumed.signaledValue<pending)p->consumed.signaledValue=pending;
 p->drained=true;
 return 1;
}
extern "C" void klj_xr_event(void *value,void *window,const void *event,int appHandled){auto p=static_cast<Owner*>(value);if(!p||p!=owner||!p->started||p->retired)return;p->controls.event(static_cast<SDL_Window*>(window),*static_cast<const SDL_Event*>(event),appHandled!=0);}
extern "C" void klj_xr_controls(void *value,void *window,int enabled){auto p=static_cast<Owner*>(value);if(!p||p!=owner||!p->started||p->retired)return;p->window=static_cast<SDL_Window*>(window);p->controls.tick(p->window,enabled!=0);}
extern "C" void klj_xr_detach(void *value){
 auto p=static_cast<Owner*>(value);if(!p||p!=owner)return;if(!klj_xr_retire(p))return;klj_xr_compositor_stop();owner=nullptr;delete p;
}
