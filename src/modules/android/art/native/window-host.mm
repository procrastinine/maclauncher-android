#include <pthread.h>
#import <QuartzCore/CATransaction.h>
#include <cstdio>
#include "bridge.h"
#include "session-core.h"
#include "window-physical.h"
#include "display-snapshot.h"
#include "window-input.h"
#include "application-sleep.h"
#include "resolution.h"
#include "network-policy.h"
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <vector>
#include "clock/display-clock.h"
extern "C" int klj_art_webkit_open(const char*,int,int,void*);
extern "C" int klj_art_webkit_poll();
extern "C" void klj_art_webkit_close();
namespace {
void retireFrames();
struct Window {KLSession session{};KLApplicationSleep*sleep=nullptr;SDL_Window*window=nullptr;SDL_MetalView layer=nullptr;};
Window state;
struct Mode {int width=0,height=0,pointsWidth=0,pointsHeight=0;};
Mode modes[2];int pendingMode=-1;
KLDisplayClock* displayClock=nullptr;
// The launcher passes this write-only capability directly to the app's original
// MessageQueue. One pending record retains SDL ordering under pipe backpressure.
using Input=KLWindowInput;
static_assert(sizeof(Input)==32);
std::vector<unsigned char> pendingInput;size_t pendingOffset=0;
int flushInput(){
 while(pendingOffset<pendingInput.size()){
  ssize_t written=write(3,pendingInput.data()+pendingOffset,pendingInput.size()-pendingOffset);
  if(written>0){pendingOffset+=size_t(written);continue;}
  if(written<0&&errno==EINTR)continue;
  if(written<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;
  return -1;
 }return 1;
}
int sendInput(Input value,const char*text=nullptr,int start=0,int length=0){
 size_t bytes=text?strlen(text):0;if(bytes>INT32_MAX-8)return -1;
 if(text)value.code=int(bytes+8);
 if(pendingOffset){pendingInput.erase(pendingInput.begin(),pendingInput.begin()+pendingOffset);pendingOffset=0;}
 size_t offset=pendingInput.size();pendingInput.resize(offset+sizeof(Input)+(text?bytes+8:0));memcpy(pendingInput.data()+offset,&value,sizeof(value));
 if(text){memcpy(pendingInput.data()+offset+sizeof(Input),&start,4);memcpy(pendingInput.data()+offset+sizeof(Input)+4,&length,4);memcpy(pendingInput.data()+offset+sizeof(Input)+8,text,bytes);}
 return flushInput();
}

int video(void*){@autoreleasepool{SDL_SetHintWithPriority(SDL_HINT_MAC_BACKGROUND_APP,"1",SDL_HINT_OVERRIDE);SDL_SetHintWithPriority(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN,"0",SDL_HINT_OVERRIDE);SDL_SetHintWithPriority(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED,"0",SDL_HINT_OVERRIDE);const char*title=getenv("MACLAUNCHER_ANDROID_TITLE"),*icon=getenv("MACLAUNCHER_ANDROID_ICON");NSData*png=icon?[[NSData alloc]initWithContentsOfFile:[NSString stringWithUTF8String:icon]]:nil;return kl_application_start(title&&*title?title:"Android application",png.bytes,png.length,false);}}
int window(void*p,int width,int height){auto s=(Window*)p;const char*title=getenv("MACLAUNCHER_ANDROID_TITLE");s->window=kl_window_physical_create(title&&*title?title:"Android application",width,height,SDL_WINDOW_HIDDEN);if(!s->window)return 0;s->sleep=kl_application_sleep_observe(SDL_GetWindowID(s->window));if(!s->sleep){SDL_DestroyWindow(s->window);s->window=nullptr;return 0;}return 1;}
int layer(void*p){auto s=(Window*)p;s->layer=kl_window_physical_layer(s->window);return s->layer!=nullptr;}
int drawable(void*p,int,int,int*w,int*h){auto s=(Window*)p;return kl_window_physical_drawable(s->window,s->layer,w,h);}
int bind(void*,int,int){return 1;}
int claim(void*,KLSessionBranch branch){return branch==KL_SESSION_NATIVE;}
// The original guest buffer producer claims this same session during submission.
int stop(void*){kl_application_sleep_retire(state.sleep);state.sleep=nullptr;char error[256];if(displayClock&&!kl_display_clock_destroy(displayClock,error,sizeof(error)))return 0;displayClock=nullptr;klj_art_webkit_close();retireFrames();return 1;}int unbind(void*){return 1;}
void destroy_layer(void*p){auto s=(Window*)p;SDL_Metal_DestroyView(s->layer);s->layer=nullptr;}
void destroy_window(void*p){auto s=(Window*)p;kl_application_sleep_retire(s->sleep);s->sleep=nullptr;SDL_DestroyWindow(s->window);s->window=nullptr;}
void quit(void*){kl_application_stop();}
const KLSessionOps ops={video,window,layer,drawable,bind,claim,stop,unbind,destroy_layer,destroy_window,quit};
}
extern "C" int64_t maclauncher_art_window(uint64_t address,uint64_t size){@autoreleasepool{
 if(!address||size!=sizeof(KljArtWindow))return -22;
 auto p=(KljArtWindow*)(uintptr_t)address;uintptr_t owner=(uintptr_t)pthread_self();
 if(!pthread_main_np()){snprintf(p->error,sizeof(p->error),"Window requires its main thread");return -1;}
 if(p->operation==1){
  if(state.window){snprintf(p->error,sizeof(p->error),"Window is already open");return -1;}
  if(!kl_session_init(&state.session,&ops,&state,owner)||!kl_session_open(&state.session,owner,p->width,p->height)){
   snprintf(p->error,sizeof(p->error),"%s",SDL_GetError());kl_session_close(&state.session,owner);return -1;
  }
  p->out_width=state.session.width;p->out_height=state.session.height;return 0;
 }
 if(p->operation==3){
  KLDisplaySnapshot snapshot{};void*cocoa=SDL_GetPointerProperty(SDL_GetWindowProperties(state.window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);
  if(!kl_display_snapshot_read(cocoa,&snapshot,p->error,sizeof(p->error)))return -1;
  p->screen=snapshot.screenId;p->out_width=state.session.width;p->out_height=state.session.height;p->refresh=snapshot.refreshRate;p->density=state.session.width/snapshot.windowWidth;return 0;
 }
 if(p->operation==2&&state.session.state==KL_SESSION_CLAIMED){if(kl_session_release(&state.session,owner,KL_SESSION_NATIVE,1)!=1)return -1;}
 if(p->operation==2)return kl_session_close(&state.session,owner)==1?0:-1;
 return -38;
}}

#include "shared-presentation.h"
#include "target-time.h"
#include "../../native/frame-observation.hpp"
#include <dlfcn.h>
#include <unistd.h>
namespace {
struct FrameOwner {
 void*library=nullptr,*presenter=nullptr;
 id<MTLCommandQueue> queue;
 void*(*create)(void*,void*,const RSPresenterOptions*,RSPresenterError*)=nullptr;
 int(*submit)(void*,const RSPresenterSubmission*,RSFrameResult*)=nullptr;
 int(*status)(void*,RSPresenterStatus*)=nullptr;
 int(*submitAt)(void*,const RSPresenterSubmission*,const RSPresentationTarget*,RSFrameResult*)=nullptr;
 int(*submitObserved)(void*,const RSPresenterSubmission*,const RSPresentationTarget*,const RSPresentationObserver*,RSFrameResult*)=nullptr;
 int(*timing)(void*,RSPresentationTiming*)=nullptr;
 void(*destroy)(void*)=nullptr;
 uint64_t next=0,generation=0,accepted=0,busy=0;
 klj_timing::Reporter observation;
} frames;
struct FrameMeasurement {
 const KljArtFrame *frame;
 uint64_t started=uint64_t(CACurrentMediaTime()*1e9);
 ~FrameMeasurement(){frames.observation.record(started,uint64_t(CACurrentMediaTime()*1e9),"surfaceflinger","composition",
  {frames.presenter,frames.status,frames.generation,frames.accepted,frames.busy,frame->width,frame->height,unsigned(state.session.width),unsigned(state.session.height)});}
};
void traceFrames(){
 if(!getenv("MACLAUNCHER_ART_PRESENTATION_TRACE")||!frames.presenter)return;
 static double last=0;double now=CACurrentMediaTime();if(now-last<1)return;last=now;
 RSPresenterStatus status{sizeof(status),RS_PRESENTER_ABI_VERSION};frames.status(frames.presenter,&status);
 NSWindow*window=(__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(state.window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);
 CAMetalLayer*layer=(__bridge CAMetalLayer*)SDL_Metal_GetLayer(state.layer);
 fprintf(stderr,"ART presentation submitted=%llu completed=%llu presented=%llu dropped=%llu pending=%u visible=%d occluded=%d active=%d policy=%ld hidden=%d extent=%.0fx%.0f reason=%s\n",status.submitted,status.completedReal,status.presentedReal,status.dropped,status.pending,window.visible,!(window.occlusionState&NSWindowOcclusionStateVisible),NSApp.active,(long)NSApp.activationPolicy,layer.hidden,layer.bounds.size.width,layer.bounds.size.height,status.reason);
}
void tracePixels(const KljArtFrame*p){
 const char*file=getenv("MACLAUNCHER_ART_FRAME_CAPTURE");if(!file||!*file)return;
 static double last=0;double now=CACurrentMediaTime();if(now-last<1)return;last=now;
 unsigned char*bytes=(unsigned char*)(uintptr_t)p->pixels;
 NSBitmapImageRep*image=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:&bytes pixelsWide:p->width pixelsHigh:p->height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:size_t(p->stride)*4 bitsPerPixel:32];
 NSData*png=[image representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
 if(png&&![png writeToFile:[NSString stringWithUTF8String:file] atomically:YES])fprintf(stderr,"ART imported-frame capture failed\n");
}
void retireFrames(){if(frames.presenter){RSPresenterStatus status{sizeof(status),RS_PRESENTER_ABI_VERSION};RSPresentationTiming time{sizeof(time),RS_TARGET_ABI_VERSION};frames.status(frames.presenter,&status);frames.timing(frames.presenter,&time);NSWindow*window=(__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(state.window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);CAMetalLayer*layer=(__bridge CAMetalLayer*)SDL_Metal_GetLayer(state.layer);fprintf(stderr,"Shared presentation retirement: encoded=%llu completed=%llu presented=%llu dropped=%llu skipped=%llu pending=%u visible=%d occluded=%d transaction=%d reason=%s\n",status.encoded,status.completedReal,status.presentedReal,status.dropped,time.skipped,status.pending,window.visible,!(window.occlusionState&NSWindowOcclusionStateVisible),layer.presentsWithTransaction,status.reason);frames.destroy(frames.presenter);frames.presenter=nullptr;}frames.queue=nil;}
}
static int commitViewport(uint32_t width,uint32_t height);
static int64_t submitFrame(uint64_t address,uint64_t size,void*token,void(*release)(void*),bool wait,const RSPresentationObserver*observer=nullptr){@autoreleasepool{
 if(!address||size!=sizeof(KljArtFrame))return -22;auto*p=reinterpret_cast<KljArtFrame*>(address);
 auto fail=[&](const char*message){snprintf(p->error,sizeof(p->error),"%s",message);return int64_t(-1);};
 if(!pthread_main_np()||!state.layer)return fail("Frame requires the existing main-thread window");
 if(p->format!=1||!p->width||!p->height||p->stride<p->width||p->stride>UINT32_MAX/4)return fail("Unsupported source buffer descriptor");
 FrameMeasurement measurement{p};
 size_t row=size_t(p->stride)*4,bytes=row*p->height;
 if(bytes/row!=p->height||bytes>p->length||p->pixels%getpagesize()||p->length%getpagesize())return fail("Shared buffer mapping is invalid");
 tracePixels(p);
 CAMetalLayer*layer=(__bridge CAMetalLayer*)SDL_Metal_GetLayer(state.layer);id<MTLDevice> device=layer.device;
 if(!frames.library){const char*path=getenv("KLEPTON_FRAME_LIBRARY");if(!path)return fail("Shared frame library is not configured");frames.library=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!frames.library)return fail(dlerror());
  frames.create=(decltype(frames.create))dlsym(frames.library,"rsCreateFramePresenter");frames.submit=(decltype(frames.submit))dlsym(frames.library,"rsSubmitFramePresenter");frames.status=(decltype(frames.status))dlsym(frames.library,"rsGetFramePresenterStatus");frames.destroy=(decltype(frames.destroy))dlsym(frames.library,"rsDestroyFramePresenter");
  frames.submitAt=(decltype(frames.submitAt))dlsym(frames.library,"rsSubmitFramePresenterAtTime");frames.timing=(decltype(frames.timing))dlsym(frames.library,"rsGetFramePresenterTiming");
  frames.submitObserved=(decltype(frames.submitObserved))dlsym(frames.library,"rsSubmitFramePresenterAtTimeObserved");
  if(!frames.create||!frames.submit||!frames.status||!frames.destroy||!frames.submitAt||!frames.timing)return fail("Shared presenter ABI is unavailable");}
 if(state.session.state==KL_SESSION_READY&&!kl_session_claim(&state.session,(uintptr_t)pthread_self(),KL_SESSION_NATIVE,state.session.width,state.session.height))return fail("Shared session cannot adopt this producer");
 if(!frames.presenter){RSPresenterOptions options{sizeof(options),RS_PRESENTER_ABI_VERSION,RS_FRAME_TOP_LEFT,0,0};RSPresenterError error{sizeof(error),RS_PRESENTER_ABI_VERSION,{}};frames.presenter=frames.create((__bridge void*)layer,&state,&options,&error);if(!frames.presenter)return fail(error.reason);frames.queue=[device newCommandQueue];++frames.generation;}
 // The guest holds the original acquired Image/HardwareBuffer until this bounded
 // proof completes. The same memory is sampled by Metal; no JNI/pixel copy loop.
 id<MTLBuffer> memory=[device newBufferWithBytesNoCopy:reinterpret_cast<void*>(p->pixels) length:p->length options:MTLResourceStorageModeShared deallocator:nil];if(!memory)return fail("Metal cannot import the shared buffer mapping");
 auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:p->width height:p->height mipmapped:NO];descriptor.usage=MTLTextureUsageShaderRead;descriptor.storageMode=MTLStorageModeShared;
 id<MTLTexture> texture=[memory newTextureWithDescriptor:descriptor offset:0 bytesPerRow:row];if(!texture)return fail("Metal cannot import the original allocator layout");
 if(commitViewport(p->width,p->height))return fail("Native display mode could not commit its rendered buffer");
 id<MTLCommandBuffer> command=[frames.queue commandBuffer];RSPresenterSubmission submission{sizeof(submission),RS_PRESENTER_ABI_VERSION,(__bridge void*)command,(__bridge void*)texture,++frames.next,CACurrentMediaTime(),token,release};RSFrameResult result{sizeof(result),RS_FRAME_ABI_VERSION};
 RSPresentationTarget target{sizeof(target),RS_TARGET_ABI_VERSION,CACurrentMediaTime()};
 if(observer&&!frames.submitObserved)return fail("Shared presentation retirement API is unavailable");
 int status=observer?frames.submitObserved(frames.presenter,&submission,&target,observer,&result):frames.submitAt(frames.presenter,&submission,&target,&result);if(status!=RS_FRAME_OK){if(status==RS_FRAME_BUSY)++frames.busy;return fail(result.reason);}
 if(result.encoded)++frames.accepted;
 [command commit];if(wait)[command waitUntilCompleted];if(wait&&command.status!=MTLCommandBufferStatusCompleted)return fail(command.error.localizedDescription.UTF8String?:"Metal frame failed");
 // Synchronous wait is confined to this one-frame proof. Normal frame ownership
 // uses the shared presenter's existing completion/lifetimeToken interface.
 RSPresenterStatus snapshot{sizeof(snapshot),RS_PRESENTER_ABI_VERSION};frames.status(frames.presenter,&snapshot);p->frame=submission.frameId;p->completed=snapshot.completedReal;p->presented=snapshot.presentedReal;
 return 0;
}}

extern "C" int64_t maclauncher_art_frame(uint64_t address,uint64_t size){return submitFrame(address,size,nullptr,nullptr,true);}
// Physical compositor transport only. Original SurfaceFlinger owns every layer,
// transaction and buffer queue; this process owns the Mac window and its clock.
struct KljArtCompositor {uint32_t width,height,dpiX,dpiY;uint64_t interval,frame,presentedNanos,completed,screen;};
static KljArtCompositor displayInfo{};
static int commitViewport(uint32_t width,uint32_t height){
 if(pendingMode<0)return 0;const auto&mode=modes[pendingMode];
 if(width!=uint32_t(mode.width)||height!=uint32_t(mode.height))return 0;
 CAMetalLayer*layer=(__bridge CAMetalLayer*)SDL_Metal_GetLayer(state.layer);if(!layer)return -1;
 layer.drawableSize=CGSizeMake(width,height);
 if(!kl_session_commit(&state.session,(uintptr_t)pthread_self(),KL_SESSION_NATIVE,width,height))return -1;
 displayInfo.width=width;displayInfo.height=height;pendingMode=-1;
 return sendInput({9,int32_t(width),int32_t(height),int32_t(displayInfo.screen),float(1e9/displayInfo.interval),float(displayInfo.dpiX/160000.0)})<0?-1:0;
}
extern "C" int maclauncher_art_compositor_mode(uint32_t config,int commit,KljArtCompositor*out){@autoreleasepool{
 if(!pthread_main_np()||!state.window||!out||config>1)return -1;
 const Mode&mode=modes[config];if(mode.width<1||mode.height<1)return -1;
 if(commit){
  int width=0,height=0;
  if(!SDL_GetWindowSize(state.window,&width,&height))return -1;
  if(!kl_window_physical_place(state.window,mode.pointsWidth,mode.pointsHeight))return -1;
  int backingWidth=0,backingHeight=0;
  if(!SDL_GetWindowSizeInPixels(state.window,&backingWidth,&backingHeight)||backingWidth!=mode.width||backingHeight!=mode.height){kl_window_physical_place(state.window,width,height);return -1;}
  pendingMode=int(config);
 }
 *out=displayInfo;out->width=mode.width;out->height=mode.height;return 0;
}}
extern "C" int maclauncher_art_compositor_open(KljArtCompositor*out,int visible,int browserSocket){
 const char*ads=getenv("MACLAUNCHER_ANDROID_ALLOW_ADS");if(!ads||!klj_network_configure_ads(!strcmp(ads,"1")))return -1;
 auto dimension=[](const char*name,unsigned fallback,unsigned minimum,unsigned maximum){const char*text=getenv(name);if(!text)return fallback;char*end=nullptr;unsigned long value=strtoul(text,&end,10);return end&&!*end&&value>=minimum&&value<=maximum?unsigned(value):0;};
 const unsigned width=dimension("MACLAUNCHER_ART_WIDTH",1280,64,8192),height=dimension("MACLAUNCHER_ART_HEIGHT",800,64,8192),density=dimension("MACLAUNCHER_ART_DENSITY",320,72,1000);if(!width||!height||!density)return -1;
 KljArtWindow w{.operation=1,.width=width,.height=height};if(maclauncher_art_window((uintptr_t)&w,sizeof(w)))return -1;
 const char*browser=getenv("MACLAUNCHER_ART_BROWSER_SOCKET");
 void*parent=(__bridge void*)((__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(state.window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr)).contentView;
 if(!browser||klj_art_webkit_open(browser,browserSocket,5,parent))return -1;
 if(visible)SDL_ShowWindow(state.window);[CATransaction flush];
 // A spawned child's stdio capabilities arrive in blocking mode even when the
 // parent opened the pipe nonblocking. Both event-loop directions must set
 // their own mode before polling; an idle text-control pipe must not stop VSYNC.
 for(int fd:{3,4}){int flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return -1;}
 pendingInput.clear();pendingOffset=0;if(sendInput({1,(SDL_GetWindowFlags(state.window)&SDL_WINDOW_INPUT_FOCUS)?1:0})<0)return -1;
 void*cocoa=SDL_GetPointerProperty(SDL_GetWindowProperties(state.window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);
 char error[256];displayClock=kl_display_clock_create(cocoa,error,sizeof(error));if(!displayClock){fprintf(stderr,"%s\n",error);return -1;}
 w.operation=3;if(maclauncher_art_window((uintptr_t)&w,sizeof(w)))return -1;
 int pointsWidth=0,pointsHeight=0;if(!SDL_GetWindowSize(state.window,&pointsWidth,&pointsHeight)||pointsWidth<1||pointsHeight<1)return -1;
 modes[0]={int(w.out_width),int(w.out_height),pointsWidth,pointsHeight};
 auto alternate=kl_window_physical_placement(state.window,pointsHeight,pointsWidth);
 modes[1]={int(lround(double(alternate.width)*w.out_width/pointsWidth)),int(lround(double(alternate.height)*w.out_height/pointsHeight)),alternate.width,alternate.height};pendingMode=-1;
 *out=displayInfo={w.out_width,w.out_height,density*1000,density*1000,uint64_t(1000000000.0/w.refresh),0,0,0,w.screen};return 0;
}
extern "C" int maclauncher_art_compositor_vsync(int enabled){char error[256];return kl_display_clock_set_demand(displayClock,enabled,error,sizeof(error))?0:-1;}
extern "C" int maclauncher_art_compositor_poll(KLDisplayTick*tick,KljArtCompositor*out){@autoreleasepool{
 if(klj_art_webkit_poll())return -1;
 struct TextControl {int32_t kind;float bounds[4];};static TextControl control{};static size_t controlRead=0;
 for(;;){ssize_t count=read(4,reinterpret_cast<char*>(&control)+controlRead,sizeof(control)-controlRead);
  if(count<0&&errno==EINTR)continue;if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;if(count<=0)return -1;
  controlRead+=size_t(count);if(controlRead!=sizeof(control))continue;controlRead=0;
  if(control.kind==2){if(!kl_window_physical_text_area(state.window,state.session.width,state.session.height,control.bounds[0],control.bounds[1],control.bounds[2],control.bounds[3]))return -1;}
  else if(control.kind==0||control.kind==1)kl_window_input_text_mode(state.window,control.kind==1);
  else return -1;
 }
 int input=flushInput();if(input<0)return -1;
 SDL_Event event;unsigned owned=SDL_GetWindowID(state.window);
 while(input>0&&SDL_PollEvent(&event)){
  int sleeping=0;if(kl_application_sleep_event(&event,owned,&sleeping)){input=sendInput({KL_INPUT_SLEEP,sleeping});if(input<0)return -1;continue;}
  KLWindowEvent decoded;kl_window_input_decode(&event,owned,SDL_TextInputActive(state.window),&decoded);auto value=decoded.value;
  if(value.kind==KL_INPUT_NONE||value.kind==KL_INPUT_CONSUMED)continue;
  if((value.kind==KL_INPUT_TOUCH||value.kind==KL_INPUT_WHEEL)&&!kl_window_input_coordinates(state.window,state.session.width,state.session.height,&value.x,&value.y))return -1;
  input=sendInput(value,decoded.text,decoded.start,decoded.length);if(input<0)return -1;
 }
 [CATransaction flush];
 if(frames.presenter){RSPresentationTiming time{sizeof(time),RS_TARGET_ABI_VERSION};RSPresenterStatus status{sizeof(status),RS_PRESENTER_ABI_VERSION};frames.timing(frames.presenter,&time);frames.status(frames.presenter,&status);out->frame=time.lastPresentedFrame;out->presentedNanos=uint64_t(time.lastPresentedTime*1000000000.0);out->completed=status.completedReal;}
 traceFrames();
 out->width=displayInfo.width;out->height=displayInfo.height;
 return kl_display_clock_poll(displayClock,tick);
}}
extern "C" int64_t maclauncher_art_compositor_submit(KljArtFrame*frame,void*token,void(*release)(void*),void*context,void(*retired)(void*,uint64_t,double,double,uint32_t)){RSPresentationObserver observer{sizeof(observer),RS_TARGET_ABI_VERSION,context,retired};return submitFrame((uintptr_t)frame,sizeof(*frame),token,release,false,&observer);}
extern "C" void maclauncher_art_compositor_close(){KljArtWindow w{.operation=2};maclauncher_art_window((uintptr_t)&w,sizeof(w));}
