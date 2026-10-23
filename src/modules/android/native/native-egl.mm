#include <jni.h>
#include <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <memory>
#include <pthread.h>
#include <vector>
#include <algorithm>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <unordered_map>
#include "egl-cleanup.hpp"
#include "target-clock.hpp"
#include "extension-filter.hpp"
#include "xr-context.h"
#include "xr-presentation.h"
#include "surface-texture.hpp"
#include "surface-format.hpp"
#include "native-egl-cache.hpp"
#include "native-egl-trace.hpp"
#include "guest-abi.h"
#include "composition-target.h"
#include "egl-image-pool.hpp"
#include "presentation.h"
#include "angle-debug-callback.h"

extern "C" void *klj_surface_begin_egl(void*,void**,int*,int*,int);
extern "C" void klj_surface_record_egl(void*,void*);
extern "C" void *klj_surface_find_egl(void*,void**);
extern "C" void klj_surface_end_egl(void*);
extern "C" void *klj_surface_next_egl(void*,int*,int*);
extern "C" void klj_surface_commit_egl(void*,void*);
extern "C" void klj_surface_rotate_egl(void*,void*);
extern "C" void klj_surface_account_egl_buffer(void*,bool);
extern "C" int klj_surface_acquire_native_layer(void*,void**,void**);
extern "C" void klj_surface_release_native_layer(void*);
extern "C" void *klj_native_egl_lookup(const char*);
extern "C" void *klj_native_egl_guest_lookup(const char*);
extern "C" void *klj_gles_guest_abi(const char*,void*);
namespace {
JavaVM *vm;
void *(*symbol)(const char*);
void (*enter)(void*),(*supply)(void*);
void *(*makeSurface)(void*,void*,int,int,bool,const int*,KLMetalError*);
int (*current)(void*,KLMetalError*),(*present)(void*,int,KLMetalError*),(*finish)(void*,KLMetalError*),(*detach)(KLMetalError*),(*resizePresenter)(int,int,KLMetalError*);
int (*presentRetained)(void*,int,void*,void(*)(void*),const KLMetalBufferSwap*,KLMetalError*);
int (*replaceSurface)(void*,void*,void*,int,int,KLMetalError*);
std::atomic<unsigned> frames{0};
std::atomic<uint64_t> surfaceSerial{0},offscreenSwaps{0};
std::atomic<bool> targetClockReady{false};
unsigned (*setTarget)(void*,void*,void*,int64_t)=nullptr;
uint64_t uptimeNanos(){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}
thread_local unsigned error=0;
std::mutex initializeMutex;
struct DisplayInitialization {unsigned references;int major,minor;};
std::unordered_map<void*,DisplayInitialization> initializeCounts;
unsigned initialize(void*d,int*major,int*minor){
 // Android egl_display_t owns one driver initialization across independent
 // game/SDK clients. ANGLE itself follows EGL and does not reference-count it.
 std::lock_guard lock(initializeMutex);auto existing=initializeCounts.find(d);
 if(existing!=initializeCounts.end()){auto &state=existing->second;++state.references;if(major)*major=state.major;if(minor)*minor=state.minor;return 1;}
 int actualMajor=0,actualMinor=0;
 unsigned ok=((unsigned(*)(void*,int*,int*))symbol("eglInitialize"))(d,&actualMajor,&actualMinor);
 if(ok){initializeCounts.emplace(d,DisplayInitialization{1,actualMajor,actualMinor});klj_egl_cache::install(d,symbol);if(major)*major=actualMajor;if(minor)*minor=actualMinor;}
 return ok;
}
unsigned terminate(void*d){
 std::lock_guard lock(initializeMutex);auto existing=initializeCounts.find(d);
 if(existing!=initializeCounts.end()&&existing->second.references>1){--existing->second.references;return 1;}
 unsigned ok=((unsigned(*)(void*))symbol("eglTerminate"))(d);
 if(ok&&existing!=initializeCounts.end())initializeCounts.erase(existing);
 return ok;
}
int (*platformHandle)(const void*);
void *(*guestSymbol)(void*,const char*);
char *(*guestError)();
void *(*xrSurfaceSwapchain)(void*);
int (*xrSurfacePresent)(void*);
thread_local char symbolError[256];
thread_local bool hasSymbolError=false;
void *findPlatformSymbol(void *handle,const char *name){
 if(platformHandle&&platformHandle(handle)){
  void *result=name&&(!std::strncmp(name,"gl",2)||!std::strncmp(name,"egl",3))?klj_native_egl_guest_lookup(name):nullptr;
  if(!result){std::snprintf(symbolError,sizeof symbolError,"ANGLE platform symbol unavailable: %.192s",name?name:"(null)");hasSymbolError=true;}
  return result;
 }
 return guestSymbol(handle,name);
}
char *findPlatformError(){if(hasSymbolError){hasSymbolError=false;return symbolError;}return guestError();}
struct Attached {JNIEnv *env=nullptr;bool owned=false;Attached(){if(vm&&vm->GetEnv((void**)&env,JNI_VERSION_1_8)==JNI_EDETACHED){JavaVMAttachArgs args={JNI_VERSION_1_8,(char*)"Android native EGL",nullptr};owned=vm->AttachCurrentThreadAsDaemon((void**)&env,&args)==JNI_OK;}}~Attached(){if(owned)vm->DetachCurrentThread();}};
JNIEnv *environment(){thread_local Attached thread;return thread.env;}
template<class T>T fn(const char *name){return (T)symbol(name);}
bool vulkanBackend(){const char *selected=std::getenv("KL_ANGLE_BACKEND");return selected&&!std::strcmp(selected,"vulkan");}
void *display(void *native){if(native){error=0x3008;return nullptr;}const int attributes[]={0x3203,vulkanBackend()?0x3450:0x3489,0x3038};return fn<void*(*)(unsigned,void*,const int*)>("eglGetPlatformDisplayEXT")(0x3202,nullptr,attributes);}
#include "egl-config.hpp"
// EGL window identity is stable; IOSurface/ANGLE buffers are producer-owned generations.
struct WindowImage {
 IOSurfaceRef storage;
 explicit WindowImage(IOSurfaceRef image):storage((IOSurfaceRef)CFRetain(image)){klj_surface_account_egl_buffer(storage,true);}
 ~WindowImage(){klj_surface_account_egl_buffer(storage,false);CFRelease(storage);}
};
struct WindowSurface {
 void *display,*config,*window,*presenter;std::atomic<void*> surface;
 bool texture;void *nativeLayerLease=nullptr,*composition=nullptr,*xrSwapchain=nullptr;std::vector<int> attributes;std::atomic<uintptr_t> producer{0};std::atomic<bool> destroyed{false};
 const uint64_t identity=surfaceSerial.fetch_add(1,std::memory_order_relaxed)+1;
 std::atomic<uint64_t> binds{0},swaps{0},submitted{0},failed{0},resizes{0};
 std::atomic<unsigned> lastError{0};
 std::recursive_mutex buffersMutex;klj_egl::ImagePool<WindowImage> buffers;
 std::array<void*,3> rawBuffers{};int drawing=0;bool preserved=false;
 std::shared_ptr<WindowImage> destroyedCurrentImage;
 std::vector<std::pair<int,int>> surfaceAttributes;
 WindowSurface(void*d,void*c,void*w,void*p,void*s):display(d),config(c),window(w),presenter(p),surface(s),texture(klj_surface_is_texture(w)),xrSwapchain(xrSurfaceSwapchain?xrSurfaceSwapchain(w):nullptr){}
 ~WindowSurface(){if(composition)klj_composition_release(composition);}
};
void retireBuffers(WindowSurface &owner){for(auto &raw:owner.rawBuffers)if(raw){fn<unsigned(*)(void*,void*)>("eglDestroySurface")(owner.display,raw);raw=nullptr;}for(auto &image:owner.buffers.images)image.reset();}
bool restoreSurfaceAttributes(WindowSurface &owner,void *surface){for(auto [name,value]:owner.surfaceAttributes)if(!fn<unsigned(*)(void*,void*,int,int)>("eglSurfaceAttrib")(owner.display,surface,name,value))return false;return true;}
void reportSurface(FILE *out,const WindowSurface &owner,const char *state){
 std::fprintf(out,"[android-egl] surface=%llu state=%s kind=%s current=%d binds=%llu swaps=%llu submitted=%llu failed=%llu resizes=%llu last-error=0x%x\n",
  (unsigned long long)owner.identity,state,owner.texture?"texture":"window",owner.producer.load(std::memory_order_relaxed)!=0,
  (unsigned long long)owner.binds.load(std::memory_order_relaxed),(unsigned long long)owner.swaps.load(std::memory_order_relaxed),
  (unsigned long long)owner.submitted.load(std::memory_order_relaxed),(unsigned long long)owner.failed.load(std::memory_order_relaxed),
  (unsigned long long)owner.resizes.load(std::memory_order_relaxed),owner.lastError.load(std::memory_order_relaxed));
}
std::mutex surfacesMutex;
std::unordered_map<void*,std::shared_ptr<WindowSurface>> surfaces;
thread_local std::weak_ptr<WindowSurface> boundDraw,boundRead;
std::shared_ptr<WindowSurface> windowSurface(void *surface){std::lock_guard lock(surfacesMutex);auto at=surfaces.find(surface);return at==surfaces.end()?nullptr:at->second;}
void retireDestroyed(const std::shared_ptr<WindowSurface> &owner){if(owner&&owner->destroyed&&!owner->producer){std::lock_guard lock(surfacesMutex);surfaces.erase(owner.get());}}
void *surfaceRaw(void *surface){auto owner=windowSurface(surface);return owner?(owner->destroyed?nullptr:owner->surface.load()):surface;}
void *currentSurface(int kind){
 void *raw=fn<void*(*)(int)>("eglGetCurrentSurface")(kind);auto owner=(kind==EGL_DRAW?boundDraw:boundRead).lock();
 // ANGLE can recycle a retired surface ID while its old object remains
 // current. The successful thread binding, not that ID, owns this identity.
 return raw&&owner&&owner->surface==raw?owner.get():raw;
}
void *rawBuffer(WindowSurface &owner,void *storage,int width,int height){
  int target=0;if(!fn<unsigned(*)(void*,void*,int,int*)>("eglGetConfigAttrib")(owner.display,configRaw(owner.config),0x348D,&target))return nullptr;
  auto image=static_cast<IOSurfaceRef>(storage);
  std::vector<int> attrs={0x3057,width,0x3056,height,0x345A,0,0x3081,target,0x3080,0x305E,0x345C,klj_surface_format::glType(image),0x345D,klj_surface_format::glFormat(image,bool(opaqueConfig(owner.config)))};
  attrs.insert(attrs.end(),owner.attributes.begin(),owner.attributes.end());attrs.push_back(EGL_NONE);
  return fn<void*(*)(void*,unsigned,void*,void*,const int*)>("eglCreatePbufferFromClientBuffer")(owner.display,0x3454,storage,configRaw(owner.config),attrs.data());
}
void *bufferSurface(JNIEnv *env,WindowSurface &owner,void *storage,int width,int height){
 if(owner.texture||owner.composition)return rawBuffer(owner,storage,width,height);
 enter(owner.presenter);supply(storage);KLMetalError failure;auto result=makeSurface(owner.display,configRaw(owner.config),width,height,bool(opaqueConfig(owner.config)),owner.attributes.data(),&failure);if(!result){supply(nullptr);error=failure?unsigned(failure):EGL_BAD_ALLOC;}return result;
}
bool refresh(const std::shared_ptr<WindowSurface> &owner){
 if(!owner||owner->nativeLayerLease)return true;
 if(owner->destroyed){error=EGL_BAD_SURFACE;return false;}
 std::lock_guard backing(owner->buffersMutex);
 int width=0,height=0;auto storage=klj_surface_next_egl(owner->window,&width,&height);if(!storage)return true;
 // Only the EGL producer thread replaces buffers. Other threads may query the
 // new window extent, but never steal a context or release its current image.
 auto d=fn<void*(*)()>("eglGetCurrentDisplay")();auto context=fn<void*(*)()>("eglGetCurrentContext")();
 auto draw=fn<void*(*)(int)>("eglGetCurrentSurface")(EGL_DRAW),read=fn<void*(*)(int)>("eglGetCurrentSurface")(EGL_READ);
 void *old=owner->surface.load();bool drawBound=boundDraw.lock()==owner,readBound=boundRead.lock()==owner,bound=drawBound||readBound;
 if(owner->producer.load()!=0&&owner->producer.load()!=(uintptr_t)pthread_self()){CFRelease(storage);error=EGL_BAD_ACCESS;return false;}
 KLMetalError presenterError=KL_METAL_OK;
 auto next=rawBuffer(*owner,storage,width,height);
 if(!next){CFRelease(storage);error=EGL_BAD_ALLOC;return false;}
 if(!restoreSurfaceAttributes(*owner,next)){fn<unsigned(*)(void*,void*)>("eglDestroySurface")(owner->display,next);CFRelease(storage);return false;}
 if(bound&&!fn<unsigned(*)(void*,void*,void*,void*)>("eglMakeCurrent")(d,drawBound?next:draw,readBound?next:read,context)){
  fn<unsigned(*)(void*,void*)>("eglDestroySurface")(owner->display,next);CFRelease(storage);return false;
 }
 if(!owner->texture&&!owner->composition&&!replaceSurface(owner->presenter,next,storage,width,height,&presenterError)){
  if(bound)fn<unsigned(*)(void*,void*,void*,void*)>("eglMakeCurrent")(d,draw,read,context);
  fn<unsigned(*)(void*,void*)>("eglDestroySurface")(owner->display,next);CFRelease(storage);error=presenterError?unsigned(presenterError):EGL_BAD_ACCESS;return false;
 }
 if(owner->rawBuffers[0])retireBuffers(*owner);else fn<unsigned(*)(void*,void*)>("eglDestroySurface")(owner->display,old);
 owner->surface=next;owner->resizes.fetch_add(1,std::memory_order_relaxed);klj_surface_commit_egl(owner->window,storage);
 if(!owner->texture&&!owner->composition){owner->drawing=0;owner->rawBuffers[0]=next;owner->buffers.images[0]=std::make_shared<WindowImage>((IOSurfaceRef)storage);}
 CFRelease(storage);
 if(bound&&!owner->texture&&!owner->composition&&!current(next,&presenterError)){error=presenterError?unsigned(presenterError):EGL_BAD_ACCESS;return false;}
 return true;
}
void *create(void *d,void *config,void *window,const int *attributes){
 auto env=environment();if(!env||env->ExceptionCheck()){error=EGL_BAD_ACCESS;return nullptr;}
 jobject remote=klj_surface_composition(env,window);bool remoteProducer=remote!=nullptr;
 if(vulkanBackend()&&remoteProducer){env->DeleteLocalRef(remote);error=EGL_BAD_MATCH;return nullptr;}
 if(vulkanBackend()){
  void *lease=nullptr,*layer=nullptr;
  if(!klj_surface_acquire_native_layer(window,&lease,&layer)){error=EGL_BAD_NATIVE_WINDOW;return nullptr;}
  auto raw=fn<void*(*)(void*,void*,void*,const int*)>("eglCreateWindowSurface")(d,configRaw(config),layer,attributes);
  if(!raw){klj_surface_release_native_layer(lease);return nullptr;}
  auto owner=std::make_shared<WindowSurface>(d,config,window,nullptr,raw);owner->nativeLayerLease=lease;
  void *handle=owner.get();{std::lock_guard lock(surfacesMutex);surfaces.emplace(handle,owner);}
  klj_surface_record_egl(window,handle);return handle;
 }

 int format=0;if(!attribute(d,config,EGL_NATIVE_VISUAL_ID,&format)){if(remote)env->DeleteLocalRef(remote);return nullptr;}
 void *presenter=nullptr;int width=0,height=0;auto storage=klj_surface_begin_egl(window,&presenter,&width,&height,format);if(!storage){if(remote)env->DeleteLocalRef(remote);error=EGL_BAD_NATIVE_WINDOW;return nullptr;}
 auto owner=std::make_shared<WindowSurface>(d,config,window,presenter,nullptr);
 if(remote){owner->composition=klj_composition_create(env,remote);env->DeleteLocalRef(remote);if(!owner->composition){CFRelease(storage);klj_surface_end_egl(window);error=EGL_BAD_ALLOC;return nullptr;}}
 if(attributes)for(size_t i=0;attributes[i]!=EGL_NONE;i+=2){if(attributes[i]==EGL_RENDER_BUFFER){if(attributes[i+1]!=EGL_BACK_BUFFER){CFRelease(storage);klj_surface_end_egl(window);error=EGL_BAD_MATCH;return nullptr;}continue;}owner->attributes.push_back(attributes[i]);owner->attributes.push_back(attributes[i+1]);}
 owner->attributes.push_back(EGL_NONE);
 int id=0,red=0,green=0,blue=0,alpha=0,depth=0,stencil=0,samples=0;
 attribute(d,config,EGL_CONFIG_ID,&id);attribute(d,config,EGL_RED_SIZE,&red);attribute(d,config,EGL_GREEN_SIZE,&green);attribute(d,config,EGL_BLUE_SIZE,&blue);attribute(d,config,EGL_ALPHA_SIZE,&alpha);
 attribute(d,config,EGL_DEPTH_SIZE,&depth);attribute(d,config,EGL_STENCIL_SIZE,&stencil);attribute(d,config,EGL_SAMPLES,&samples);
 int colorspace=EGL_GL_COLORSPACE_LINEAR_KHR;for(size_t i=0;owner->attributes[i]!=EGL_NONE;i+=2)if(owner->attributes[i]==EGL_GL_COLORSPACE_KHR)colorspace=owner->attributes[i+1];
 std::fprintf(stderr,"[android-egl] surface=%llu config=%d rgba=%d/%d/%d/%d depth=%d stencil=%d samples=%d visual=%d colorspace=0x%x storage=0x%08x pixels=%dx%d\n",
  (unsigned long long)owner->identity,id,red,green,blue,alpha,depth,stencil,samples,format,colorspace,(unsigned)IOSurfaceGetPixelFormat((IOSurfaceRef)storage),width,height);
 auto raw=bufferSurface(env,*owner,storage,width,height);
 if(raw&&!owner->texture&&!owner->composition){owner->rawBuffers[0]=raw;owner->buffers.images[0]=std::make_shared<WindowImage>((IOSurfaceRef)storage);}
 CFRelease(storage);
 if(!raw){klj_surface_end_egl(window);if(owner->texture||owner->composition)error=EGL_BAD_ALLOC;return nullptr;}
 owner->surface=raw;void *handle=owner.get();{std::lock_guard lock(surfacesMutex);surfaces.emplace(handle,owner);}klj_surface_record_egl(window,handle);return handle;
}
void *createPlatform(void*d,void*c,void*w,const intptr_t*attributes){std::vector<int> values;if(attributes){for(size_t i=0;attributes[i]!=EGL_NONE;i+=2){if(attributes[i]<INT32_MIN||attributes[i]>INT32_MAX||attributes[i+1]<INT32_MIN||attributes[i+1]>INT32_MAX){error=EGL_BAD_ATTRIBUTE;return nullptr;}values.push_back(int(attributes[i]));values.push_back(int(attributes[i+1]));}values.push_back(EGL_NONE);}return create(d,c,w,values.empty()?nullptr:values.data());}
unsigned makeCurrent(void *d,void *draw,void *read,void *context){
 auto drawOwner=windowSurface(draw),readOwner=windowSurface(read);
 if((drawOwner&&drawOwner->display!=d)||(readOwner&&readOwner->display!=d)){error=EGL_BAD_MATCH;return 0;}
 if(!refresh(drawOwner)||(readOwner!=drawOwner&&!refresh(readOwner)))return 0;
 auto previousDraw=windowSurface(currentSurface(EGL_DRAW)),previousRead=windowSurface(currentSurface(EGL_READ));
 unsigned ok=fn<unsigned(*)(void*,void*,void*,void*)>("eglMakeCurrent")(d,surfaceRaw(draw),surfaceRaw(read),context);if(!ok)return ok;
 if(previousDraw)previousDraw->producer=0;if(previousRead)previousRead->producer=0;
 if(drawOwner){drawOwner->producer=(uintptr_t)pthread_self();drawOwner->binds.fetch_add(1,std::memory_order_relaxed);}if(readOwner)readOwner->producer=(uintptr_t)pthread_self();
 boundDraw=drawOwner;boundRead=readOwner;
 retireDestroyed(previousDraw);if(previousRead!=previousDraw)retireDestroyed(previousRead);
 klj_glfb_current(d,context);enter(drawOwner?drawOwner->presenter:nullptr);
 KLMetalError presenterError;if(drawOwner&&drawOwner->presenter&&!current(drawOwner->surface,&presenterError)){error=presenterError?unsigned(presenterError):EGL_BAD_ACCESS;return 0;}return 1;
}
std::mutex contextConfigsMutex;
std::unordered_map<void*,std::shared_ptr<OpaqueConfig>> contextConfigs;
void *createContext(void *d,void *config,void *shared,const int *attributes){auto result=fn<void*(*)(void*,void*,void*,const int*)>("eglCreateContext")(d,configRaw(config),shared,attributes);if(result){klj_glfb_context_created(d,result,shared);if(auto owner=opaqueConfig(config)){std::lock_guard lock(contextConfigsMutex);contextConfigs[result]=owner;}}return result;}
unsigned destroyContext(void *d,void *context){unsigned okay=fn<unsigned(*)(void*,void*)>("eglDestroyContext")(d,context);if(okay){klj_glfb_context_destroyed(context);std::lock_guard lock(contextConfigsMutex);contextConfigs.erase(context);}return okay;}
unsigned queryContext(void*d,void*context,int name,int*value){auto okay=fn<unsigned(*)(void*,void*,int,int*)>("eglQueryContext")(d,context,name,value);if(okay&&name==EGL_CONFIG_ID){std::lock_guard lock(contextConfigsMutex);auto at=contextConfigs.find(context);if(at!=contextConfigs.end())*value=at->second->id;}return okay;}
unsigned querySurface(void*d,void*surface,int name,int*value){
 auto owner=windowSurface(surface);if(!owner)return fn<unsigned(*)(void*,void*,int,int*)>("eglQuerySurface")(d,surface,name,value);
 std::lock_guard backing(owner->buffersMutex);auto okay=fn<unsigned(*)(void*,void*,int,int*)>("eglQuerySurface")(d,owner->destroyed?nullptr:owner->surface.load(),name,value);
 if(okay&&name==EGL_CONFIG_ID)if(auto config=opaqueConfig(owner->config))*value=config->id;
 if(okay&&owner->rawBuffers[0]){if(name==EGL_SWAP_BEHAVIOR)*value=owner->preserved?EGL_BUFFER_PRESERVED:EGL_BUFFER_DESTROYED;
  // A dequeued buffer may contain an older generation. Zero conservatively
  // requires a complete repaint without promising preserved damaged regions.
  if(name==EGL_BUFFER_AGE_EXT)*value=0;}
 return okay;
}
unsigned surfaceAttrib(void*d,void*surface,int name,int value){
 auto owner=windowSurface(surface);if(!owner)return fn<unsigned(*)(void*,void*,int,int)>("eglSurfaceAttrib")(d,surface,name,value);
 std::lock_guard backing(owner->buffersMutex);auto okay=fn<unsigned(*)(void*,void*,int,int)>("eglSurfaceAttrib")(d,owner->destroyed?nullptr:owner->surface.load(),name,value);if(!okay)return okay;
 if(owner->rawBuffers[0]){for(auto raw:owner->rawBuffers)if(raw&&raw!=owner->surface.load())fn<unsigned(*)(void*,void*,int,int)>("eglSurfaceAttrib")(d,raw,name,value);
  auto at=std::find_if(owner->surfaceAttributes.begin(),owner->surfaceAttributes.end(),[&](auto entry){return entry.first==name;});
  if(at==owner->surfaceAttributes.end())owner->surfaceAttributes.emplace_back(name,value);else at->second=value;
  if(name==EGL_SWAP_BEHAVIOR)owner->preserved=value==EGL_BUFFER_PRESERVED;}
 return okay;
}
unsigned textureImage(void*d,void*surface,int buffer,bool release){
 auto owner=windowSurface(surface);
 if(!owner)return fn<unsigned(*)(void*,void*,int)>(release?"eglReleaseTexImage":"eglBindTexImage")(d,surface,buffer);
 // Preserve ANGLE's original window validation even though its private
 // backing is an IOSurface pbuffer. A window is never a bindable texture.
 int width=0;if(!fn<unsigned(*)(void*,void*,int,int*)>("eglQuerySurface")(d,owner->destroyed?nullptr:owner->surface.load(),EGL_WIDTH,&width))return 0;
 fn<unsigned(*)()>("eglGetError")();error=buffer==EGL_BACK_BUFFER?EGL_BAD_SURFACE:EGL_BAD_PARAMETER;return 0;
}
unsigned bindTexImage(void*d,void*s,int buffer){return textureImage(d,s,buffer,false);}
unsigned releaseTexImage(void*d,void*s,int buffer){return textureImage(d,s,buffer,true);}
void *createPbuffer(void*d,void*config,const int*attributes){if(opaqueConfig(config)){error=EGL_BAD_MATCH;return nullptr;}return fn<void*(*)(void*,void*,const int*)>("eglCreatePbufferSurface")(d,config,attributes);}
unsigned swap(void *d,void *surface){
 auto owner=windowSurface(surface);if(!owner){offscreenSwaps.fetch_add(1,std::memory_order_relaxed);return fn<unsigned(*)(void*,void*)>("eglSwapBuffers")(d,surface);}
 std::lock_guard backing(owner->buffersMutex);
 if(owner->destroyed)return fn<unsigned(*)(void*,void*)>("eglSwapBuffers")(d,nullptr);
 const uint64_t swapIndex=owner->swaps.fetch_add(1,std::memory_order_relaxed);
 if(swapIndex==0){const char *trace=std::getenv("KL_EGL_TRACE");if(trace&&*trace&&std::strcmp(trace,"0")){
  auto enabled=fn<unsigned char(*)(unsigned)>("glIsEnabled");auto query=fn<void(*)(unsigned,int*)>("glGetIntegerv");
  int framebuffer=0,source=0,destination=0,equation=0,red=0,green=0,blue=0,alpha=0;
  query(0x8CA6,&framebuffer);query(0x80C9,&source);query(0x80C8,&destination);query(0x8009,&equation);
  query(0x0D52,&red);query(0x0D53,&green);query(0x0D54,&blue);query(0x0D55,&alpha);
  std::fprintf(stderr,"[android-egl] surface=%llu first-swap framebuffer=%d rgba=%d/%d/%d/%d dither=%u blend=%u source=0x%x destination=0x%x equation=0x%x\n",(unsigned long long)owner->identity,framebuffer,red,green,blue,alpha,(unsigned)enabled(0x0BD0),(unsigned)enabled(0x0BE2),source,destination,equation);
 }}
 auto failed=[&](unsigned code){error=code;owner->lastError.store(code,std::memory_order_relaxed);owner->failed.fetch_add(1,std::memory_order_relaxed);return 0u;};
 if(d!=owner->display)return failed(EGL_BAD_DISPLAY);
 // An Android-Surface XR swapchain releases its finished GL frame here,
 // before texture publication or EGL swaps can replace the current image.
 if(owner->xrSwapchain&&(!xrSurfacePresent||xrSurfacePresent(owner->xrSwapchain)<0))return failed(EGL_BAD_SURFACE);
 if(owner->nativeLayerLease){
  // ANGLE's original Vulkan WSI presents its GPU image into the native carrier.
  // The existing layer callback submits that image to the common presenter;
  // no IOSurface upload/readback or second command submission is introduced.
  if(!fn<unsigned(*)(void*,void*)>("eglSwapBuffers")(d,owner->surface))return failed(fn<unsigned(*)()>("eglGetError")());
  owner->submitted.fetch_add(1,std::memory_order_relaxed);return 1;
 }
 if(owner->composition){auto env=environment();if(!env||env->ExceptionCheck())return failed(EGL_BAD_ACCESS);auto storage=klj_surface_copy_storage(owner->window);int accepted=env&&storage?klj_composition_present(env,owner->composition,d,storage,swapIndex+1):0;if(storage)CFRelease(storage);if(!accepted)return failed(EGL_BAD_SURFACE);}
 else if(owner->texture){auto env=environment();if(!env||env->ExceptionCheck()||!klj_texture_publish(env,d,owner->window))return failed(EGL_BAD_SURFACE);}
 else{
  enter(owner->presenter);
  auto context=fn<void*(*)()>("eglGetCurrentContext")();auto draw=fn<void*(*)(int)>("eglGetCurrentSurface")(EGL_DRAW),read=fn<void*(*)(int)>("eglGetCurrentSurface")(EGL_READ);
  void *old=owner->surface.load();if(!context||draw!=old||owner->producer.load()!=(uintptr_t)pthread_self())return failed(EGL_BAD_ACCESS);
  const int next=owner->buffers.next(owner->drawing,[&]{
   auto original=owner->buffers.images[owner->drawing]->storage;
   auto properties=@{(id)kIOSurfaceWidth:@(IOSurfaceGetWidth(original)),(id)kIOSurfaceHeight:@(IOSurfaceGetHeight(original)),
    (id)kIOSurfaceBytesPerElement:@(IOSurfaceGetBytesPerElement(original)),(id)kIOSurfaceBytesPerRow:@(IOSurfaceGetBytesPerRow(original)),(id)kIOSurfacePixelFormat:@(IOSurfaceGetPixelFormat(original))};
   auto image=IOSurfaceCreate((__bridge CFDictionaryRef)properties);if(!image)return std::shared_ptr<WindowImage>{};
   auto result=std::make_shared<WindowImage>(image);CFRelease(image);return result;
  });
  if(next<0)return failed(EGL_BAD_ACCESS);
  auto image=owner->buffers.images[next]->storage;auto &raw=owner->rawBuffers[next];
  if(!raw){
   // The public window/config remain unchanged. Only the producer-owned
   // ANGLE pbuffer backing is rotated, just as in the resize generation path.
   raw=rawBuffer(*owner,image,(int)IOSurfaceGetWidth(image),(int)IOSurfaceGetHeight(image));
   if(!raw)return failed(EGL_BAD_ALLOC);if(!restoreSurfaceAttributes(*owner,raw))return failed(EGL_BAD_ATTRIBUTE);
  }
  auto lease=new std::shared_ptr<void>(owner->buffers.retain(owner->drawing));
  struct Binding {WindowSurface *owner;void *display,*draw,*read,*context;IOSurfaceRef image;int next;};
  Binding binding{owner.get(),d,raw,read==old?raw:read,context,image,next};
  KLMetalBufferSwap handoff{raw,image,&binding,[](void *pointer){auto &b=*static_cast<Binding*>(pointer);
   if(!fn<unsigned(*)(void*,void*,void*,void*)>("eglMakeCurrent")(b.display,b.draw,b.read,b.context))return 0;
   b.owner->drawing=b.next;b.owner->surface=b.draw;klj_surface_rotate_egl(b.owner->window,b.image);return 1;
  },owner->preserved};
  KLMetalError presenterError;if(!presentRetained(owner->presenter,frames.fetch_add(1),lease,[](void *value){delete static_cast<std::shared_ptr<void>*>(value);},&handoff,&presenterError))return failed(presenterError?unsigned(presenterError):EGL_BAD_ACCESS);
 }
 owner->submitted.fetch_add(1,std::memory_order_relaxed);
 return refresh(owner)?1:failed(error);
}
unsigned damageSwap(void*d,void*surface,int*rectangles,int count,const char *name){
 if(!windowSurface(surface)){auto call=fn<unsigned(*)(void*,void*,int*,int)>(name);return call?call(d,surface,rectangles,count):swap(d,surface);}
 if(count<0||(!rectangles&&count)){error=EGL_BAD_PARAMETER;return 0;}
 // Damage permits a partial update; presenting the complete existing image is
 // always valid and retains the shared Metal/graphics pipeline.
 return swap(d,surface);
}
unsigned damageKHR(void*d,void*s,int*r,int n){return damageSwap(d,s,r,n,"eglSwapBuffersWithDamageKHR");}
unsigned damageEXT(void*d,void*s,int*r,int n){return damageSwap(d,s,r,n,"eglSwapBuffersWithDamageEXT");}
unsigned regionSwap(void*d,void*s,int n,const int*r,const char*name){
 if(!windowSurface(s)){auto call=fn<unsigned(*)(void*,void*,int,const int*)>(name);return call?call(d,s,n,r):swap(d,s);}
 if(n<0||(!r&&n)){error=EGL_BAD_PARAMETER;return 0;}return swap(d,s);
}
unsigned regionNOK(void*d,void*s,int n,const int*r){return regionSwap(d,s,n,r,"eglSwapBuffersRegionNOK");}
unsigned region2NOK(void*d,void*s,int n,const int*r){return regionSwap(d,s,n,r,"eglSwapBuffersRegion2NOK");}
unsigned postSubBuffer(void*d,void*s,int x,int y,int width,int height){
 if(!windowSurface(s)){auto call=fn<unsigned(*)(void*,void*,int,int,int,int)>("eglPostSubBufferNV");return call?call(d,s,x,y,width,height):swap(d,s);}
 if(x<0||y<0||width<0||height<0){error=EGL_BAD_PARAMETER;return 0;}return swap(d,s);
}
unsigned releaseThread(){
 auto draw=windowSurface(currentSurface(EGL_DRAW)),read=windowSurface(currentSurface(EGL_READ));
 auto okay=fn<unsigned(*)()>("eglReleaseThread")();if(okay){if(draw)draw->producer=0;if(read)read->producer=0;boundDraw.reset();boundRead.reset();retireDestroyed(draw);if(read!=draw)retireDestroyed(read);enter(nullptr);}return okay;
}
unsigned destroy(void *d,void *surface){
 auto owner=windowSurface(surface);if(!owner)return fn<unsigned(*)(void*,void*)>("eglDestroySurface")(d,surface);
 if(d!=owner->display){error=EGL_BAD_DISPLAY;return 0;}
 if(owner->destroyed)return fn<unsigned(*)(void*,void*)>("eglDestroySurface")(d,nullptr);
 auto retire=[&]{reportSurface(stderr,*owner,"retired");owner->destroyed=true;retireDestroyed(owner);if(owner->nativeLayerLease){klj_surface_record_egl(owner->window,nullptr);klj_surface_release_native_layer(owner->nativeLayerLease);owner->nativeLayerLease=nullptr;}else klj_surface_end_egl(owner->window);};
 if(owner->nativeLayerLease){unsigned okay=fn<unsigned(*)(void*,void*)>("eglDestroySurface")(d,owner->surface);if(okay)retire();return okay;}
 if(owner->texture||owner->composition){unsigned okay=fn<unsigned(*)(void*,void*)>("eglDestroySurface")(d,owner->surface);if(okay)retire();return okay;}
 enter(owner->presenter);std::lock_guard backing(owner->buffersMutex);
 KLMetalError first=KL_METAL_OK,next=KL_METAL_OK;
 if(!finish(owner->presenter,&first)){error=first?unsigned(first):EGL_BAD_ACCESS;return 0;}
 auto okay=fn<unsigned(*)(void*,void*)>("eglDestroySurface")(d,owner->surface);
 if(!okay){if(first)error=unsigned(first);return 0;}
 if(owner->producer)owner->destroyedCurrentImage=owner->buffers.images[owner->drawing];
 for(auto &raw:owner->rawBuffers)if(raw==owner->surface.load())raw=nullptr;retireBuffers(*owner);
 if(!detach(&next)){error=first?unsigned(first):next?unsigned(next):EGL_BAD_ACCESS;return 0;}
 retire();if(first||next){error=unsigned(first?first:next);return 0;}return 1;
}
#include "egl-surface-forward.h"
const char* queryString(void*d,int name){const char*value=fn<const char*(*)(void*,int)>("eglQueryString")(d,name);if(!value||name!=0x3055)return value;return klj_target::withoutExtension(value,error);}
unsigned targetError(unsigned value){fn<unsigned(*)()>("eglGetError")();error=value;return 0;}
unsigned presentationTime(void*d,void*surface,int64_t nanos){
 // Validate the real initialized ANGLE display, independently of our window map.
 if(!d||!fn<const char*(*)(void*,int)>("eglQueryString")(d,0x3054))return targetError(0x3008);
 void*presenter=nullptr;if(!surface||!klj_surface_find_egl(surface,&presenter))return targetError(0x300D);
 if(!targetClockReady.load(std::memory_order_acquire)||!setTarget)return targetError(0x3002);
 unsigned failure=setTarget(presenter,d,surfaceRaw(surface),nanos);return failure?targetError(failure):1;
}
unsigned getError(){if(error){unsigned result=error;error=0;return result;}return fn<unsigned(*)()>("eglGetError")();}
void *getProc(const char *name){if(!name||(std::strncmp(name,"gl",2)&&std::strncmp(name,"egl",3)))return nullptr;return klj_native_egl_lookup(name);}
void *getGuestProc(const char *name){if(!name||(std::strncmp(name,"gl",2)&&std::strncmp(name,"egl",3)))return nullptr;return klj_native_egl_guest_lookup(name);}
}
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *value,void*){vm=value;return JNI_VERSION_1_8;}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_configureCache(JNIEnv *env,jclass,jstring directory){
 if(!symbol||!directory)return;const char *root=env->GetStringUTFChars(directory,nullptr);if(!root)return;
 try{klj_egl_cache::configure(root,symbol("eglInitialize"));}catch(...){/* Optional driver cache; original compiler remains available. */}
 env->ReleaseStringUTFChars(directory,root);
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_configureGraphics(JNIEnv *env,jclass,jstring file){
 const char *path=env->GetStringUTFChars(file,nullptr);if(!path)return;void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL);env->ReleaseStringUTFChars(file,path);
 const char *failure=nullptr;if(!library)failure=dlerror();
 if(library){symbol=(decltype(symbol))dlsym(library,"klj_graphics_symbol");enter=(decltype(enter))dlsym(library,"klj_metal_enter");supply=(decltype(supply))dlsym(library,"klj_metal_supply_storage");makeSurface=(decltype(makeSurface))dlsym(library,"klj_metal_surface_c");current=(decltype(current))dlsym(library,"klj_metal_current_c");present=(decltype(present))dlsym(library,"klj_metal_present_c");presentRetained=(decltype(presentRetained))dlsym(library,"klj_metal_present_retained_c");replaceSurface=(decltype(replaceSurface))dlsym(library,"klj_metal_replace_surface_c");finish=(decltype(finish))dlsym(library,"klj_metal_finish_c");detach=(decltype(detach))dlsym(library,"klj_metal_detach_surface_c");setTarget=(decltype(setTarget))dlsym(library,"klj_metal_target");resizePresenter=(decltype(resizePresenter))dlsym(library,"klj_metal_resize_c");if(!symbol||!enter||!supply||!makeSurface||!current||!present||!presentRetained||!replaceSurface||!finish||!detach||!resizePresenter)failure="Native EGL presenter exports are incomplete";}
 if(failure){auto type=env->FindClass("java/lang/IllegalStateException");if(type)env->ThrowNew(type,failure);}
}
extern "C" const char *klj_native_egl_install(void *runtime,void *(*resolve)(const char*)){
 if(!symbol||!klj_glfb_configure(symbol))return "Android projection cannot borrow the current ANGLE provider";
 klj_xr_provider_install();
 targetClockReady.store(klj_target::clockContract((int(*)())dlsym(runtime,"klj_android_monotonic_contract_v1"),(int(*)(int,timespec*))resolve("clock_gettime"),uptimeNanos),std::memory_order_release);
 platformHandle=(decltype(platformHandle))dlsym(runtime,"kl_egl_is_handle");
 guestSymbol=(decltype(guestSymbol))resolve("dlsym");guestError=(decltype(guestError))resolve("dlerror");
 xrSurfaceSwapchain=(decltype(xrSurfaceSwapchain))dlsym(runtime,"kl_xr_swapchain_for_surface");
 xrSurfacePresent=(decltype(xrSurfacePresent))dlsym(runtime,"kl_xr_android_surface_present");
 return platformHandle&&guestSymbol&&guestError&&xrSurfaceSwapchain&&xrSurfacePresent?nullptr:"Guest graphics dynamic lookup exports are incomplete";
}
extern "C" void *klj_native_egl_lookup(const char *name){
 if(!symbol)return nullptr;
 if(!strcmp(name,"maclauncher_angle_blob_callback_owner_v1"))return symbol(name);
 if(!strcmp(name,"dlsym"))return guestSymbol?(void*)findPlatformSymbol:nullptr;
 if(!strcmp(name,"dlerror"))return guestError?(void*)findPlatformError:nullptr;
 struct Entry{const char *name;void *address;};const Entry entries[]={
 {"eglReleaseThread",(void*)releaseThread},{"eglPostSubBufferNV",(void*)postSubBuffer},{"eglInitialize",(void*)initialize},{"eglTerminate",(void*)terminate},
 {"eglQueryString",(void*)queryString},{"eglPresentationTimeANDROID",(void*)presentationTime},{"eglGetDisplay",(void*)display},{"eglChooseConfig",(void*)choose},{"eglGetConfigAttrib",(void*)attribute},
 {"eglGetConfigs",(void*)configs},{"eglQueryContext",(void*)queryContext},{"eglQuerySurface",(void*)querySurface},{"eglSurfaceAttrib",(void*)surfaceAttrib},{"eglBindTexImage",(void*)bindTexImage},{"eglReleaseTexImage",(void*)releaseTexImage},{"eglCreatePbufferSurface",(void*)createPbuffer},{"eglCreateContext",(void*)createContext},{"eglDestroyContext",(void*)destroyContext},
 {"eglCreateWindowSurface",(void*)create},{"eglCreatePlatformWindowSurfaceEXT",(void*)create},{"eglCreatePlatformWindowSurface",(void*)createPlatform},{"eglGetCurrentSurface",(void*)currentSurface},
 {"eglSwapBuffersWithDamageKHR",(void*)damageKHR},{"eglSwapBuffersWithDamageEXT",(void*)damageEXT},{"eglSwapBuffersRegionNOK",(void*)regionNOK},{"eglSwapBuffersRegion2NOK",(void*)region2NOK},{"eglMakeCurrent",(void*)makeCurrent},{"eglSwapBuffers",(void*)swap},{"eglDestroySurface",(void*)destroy},{"eglGetError",(void*)getError},{"eglGetProcAddress",(void*)getProc}};
 for(auto &entry:entries)if(!strcmp(name,entry.name))return entry.address;
 if(auto forward=surfaceForward(name))return forward;
 if(auto traced=klj_egl_trace::lookup(name,symbol))return traced;
 if(!strncmp(name,"gl",2)||!strncmp(name,"egl",3))return symbol(name);
 return nullptr;
}
extern "C" void *klj_native_egl_guest_lookup(const char *name){
 // ARM32 imports already reach Darwin through the original typed libffi
 // bridge. AAPCS64 repacking belongs only to native ARM64 ELF calls.
 if(!klj_guest_is64())return klj_native_egl_lookup(name);
 if(!std::strcmp(name,"eglGetProcAddress"))return symbol?(void*)getGuestProc:nullptr;
 auto entry=klj_native_egl_lookup(name);
 return !std::strncmp(name,"gl",2)?klj_gles_guest_abi(name,entry):entry;
}

extern "C" int klj_native_egl_init_count(void*display){std::lock_guard lock(initializeMutex);auto i=initializeCounts.find(display);return i==initializeCounts.end()?0:(int)i->second.references;}
extern "C" void klj_native_egl_error(unsigned value){error=value;}
extern "C" KljAngleDebugOwnerInstall klj_native_egl_debug_owner(){
 if(!symbol)return nullptr;void *entry=symbol("maclauncher_angle_debug_callback_owner_v1");
 KljAngleDebugOwnerInstall result=nullptr;static_assert(sizeof(result)==sizeof(entry));std::memcpy(&result,&entry,sizeof(result));return result;
}

extern "C" void klj_native_egl_report(FILE *out){
 if(!out)return;std::lock_guard lock(surfacesMutex);
 std::fprintf(out,"[android-egl] live-surfaces=%zu offscreen-swaps=%llu\n",surfaces.size(),(unsigned long long)offscreenSwaps.load(std::memory_order_relaxed));
 for(const auto &[handle,owner]:surfaces){(void)handle;reportSurface(out,*owner,"live");}
}
