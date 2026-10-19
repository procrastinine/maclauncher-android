#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <stdint.h>
#include <limits.h>
#include <string>
#include <memory>
#include "window-session.h"
#include "display-adapter.h"
#include "scheduler.h"
#include "host-callbacks.h"
#include "resolution.h"
#include "window-physical.h"
#include "window-input.h"
#include "application-sleep.h"
extern "C" void klj_webkit_window_detach(void*);
extern "C" {
#include "window.h"
}
@interface MLAndroidBackAction : NSObject<NSMenuItemValidation>
@property(nonatomic,weak) NSWindow *window;
@property(nonatomic) BOOL pending;
- (void)goBack:(id)sender;
@end
@implementation MLAndroidBackAction
- (BOOL)validateMenuItem:(NSMenuItem*)item {NSWindow *key=NSApp.keyWindow;for(int i=0;key&&i<18;i++,key=key.parentWindow)if(key==self.window)return YES;return NO;}
- (void)goBack:(id)sender {if([self validateMenuItem:sender])self.pending=YES;}
@end
namespace {
struct Owner {KLSession state{};KLApplicationSleep* sleep=nullptr;int numerator=1,denominator=1;JNIEnv*env=nullptr;uint64_t identity=0;SDL_Window*window=nullptr;SDL_MetalView view=nullptr;KLDisplayAdapter*display=nullptr;KLHostScheduler*scheduler=nullptr;void* pumpContext=nullptr;KLHostTaskPump pump=nullptr;jobject activity=nullptr;KLHostCallbacks* callbacks=nullptr;KLHost_maclauncher_android_framework_HostActivity lifecycle{};__strong MLAndroidBackAction*backAction=nil;__strong NSMenuItem*backMenu=nil;std::string label;~Owner(){kl_host_callbacks_release(env,callbacks);}};
Owner*current=nullptr;uint64_t nextIdentity=1;
void fail(JNIEnv*e,const char*s){if(e->ExceptionCheck())return;jclass c=e->FindClass("java/lang/IllegalStateException");if(c){e->ThrowNew(c,s);e->DeleteLocalRef(c);}}
void collect(JNIEnv*e,KLHostCallbacks*callbacks,jthrowable&first){auto next=e->ExceptionOccurred();if(!next)return;e->ExceptionClear();if(!first){first=next;return;}if(!e->IsSameObject(first,next)){kl_host_suppressed(e,callbacks,kl_host_bind_KLHost_java_lang_Throwable(e,callbacks,first),kl_host_bind_KLHost_java_lang_Throwable(e,callbacks,next));if(e->ExceptionCheck())e->ExceptionClear();}e->DeleteLocalRef(next);}
void restore(JNIEnv*e,jthrowable first){if(first){e->Throw(first);e->DeleteLocalRef(first);}}
Owner*owns(JNIEnv*e,jlong identity=0){if(!pthread_main_np()||!current||current->env!=e||(identity&&uint64_t(identity)!=current->identity)){fail(e,"Native window session owner is unavailable");return nullptr;}return current;}
bool processVideo=false,processHeadless=false;
int video(void*p){auto s=static_cast<Owner*>(p);if(!processVideo){fail(s->env,"Application window services are not initialized");return 0;}kl_application_show();return 1;}
bool placeWindow(Owner*s,int width,int height){return kl_window_physical_place(s->window,width,height);}
int window(void*p,int w,int h){auto s=static_cast<Owner*>(p);s->window=kl_window_physical_create(s->label.c_str(),w,h);if(!s->window){fail(s->env,SDL_GetError());return 0;}s->sleep=kl_application_sleep_observe(SDL_GetWindowID(s->window));if(!s->sleep){SDL_DestroyWindow(s->window);s->window=nullptr;fail(s->env,"Application sleep notifications are unavailable");return 0;}return 1;}
int layer(void*p){auto s=static_cast<Owner*>(p);s->view=kl_window_physical_layer(s->window);if(!s->view){fail(s->env,SDL_GetError());return 0;}return 1;}
int drawable(void*p,int,int,int*outW,int*outH){auto s=static_cast<Owner*>(p);if(!kl_window_physical_drawable(s->window,s->view,outW,outH)){fail(s->env,SDL_GetError());return 0;}return 1;}

int bind(void*p,int w,int h){auto s=static_cast<Owner*>(p);if(!s->display)s->display=kl_display_adapter_create(s->env);if(!s->display)return 0;void*cocoa=SDL_GetPointerProperty(SDL_GetWindowProperties(s->window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);
 if(!kl_display_adapter_update(s->env,s->display,cocoa,klj_resolution_dimension(w,s->numerator,s->denominator),klj_resolution_dimension(h,s->numerator,s->denominator)))return 0;
 if(!s->scheduler){s->scheduler=kl_host_scheduler_create(s->env,s->callbacks);if(!s->scheduler)return 0;return kl_host_scheduler_window(s->env,s->scheduler,cocoa);}return 1;}
int claim(void*p,KLSessionBranch){auto s=static_cast<Owner*>(p);if(!SDL_SetWindowResizable(s->window,true)){fail(s->env,SDL_GetError());return 0;}return 1;}
void retireBack(Owner*s){if(s->backMenu){[NSApp.mainMenu removeItem:s->backMenu];s->backMenu=nil;}s->backAction=nil;if(s->activity){s->env->DeleteGlobalRef(s->activity);s->activity=nullptr;}if(s->lifecycle.object){s->env->DeleteGlobalRef(s->lifecycle.object);s->lifecycle={};}}
int stop(void*p){auto s=static_cast<Owner*>(p);kl_application_sleep_retire(s->sleep);s->sleep=nullptr;retireBack(s);jthrowable first=nullptr;collect(s->env,s->callbacks,first);
 int displayRetired=s->display?kl_display_adapter_stop(s->env,s->display):1;collect(s->env,s->callbacks,first);
 int framesRetired=s->scheduler?kl_host_scheduler_quiesce_pump(s->env,s->scheduler,1000,s->pump,s->pumpContext):1;collect(s->env,s->callbacks,first);
 bool failed=first!=nullptr;restore(s->env,first);if(failed||!framesRetired)return 0;return displayRetired?1:KL_SESSION_BUSY;}
int unbind(void*p){auto s=static_cast<Owner*>(p);if(s->scheduler){if(!kl_host_scheduler_destroy(s->env,s->scheduler))return 0;s->scheduler=nullptr;}if(s->display){if(!kl_display_adapter_destroy(s->env,s->display))return 0;s->display=nullptr;}return 1;}
void destroyLayer(void*p){auto s=static_cast<Owner*>(p);SDL_Metal_DestroyView(s->view);s->view=nullptr;}
void destroyWindow(void*p){auto s=static_cast<Owner*>(p);kl_application_sleep_retire(s->sleep);s->sleep=nullptr;retireBack(s);klj_webkit_window_detach(SDL_GetPointerProperty(SDL_GetWindowProperties(s->window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr));SDL_DestroyWindow(s->window);s->window=nullptr;if(processHeadless)[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];}
// Windows borrow the process's SDL/AppKit owner. Closing the last window must
// not retire the application event loop while Android services are still live.
void quit(void*){}
const KLSessionOps ops={video,window,layer,drawable,bind,claim,stop,unbind,destroyLayer,destroyWindow,quit};
bool title(JNIEnv*e,jstring value,std::string&out){if(!value){fail(e,"Native window title is unavailable");return false;}jsize length=e->GetStringLength(value);if(length>16384){fail(e,"Native window title is too long");return false;}const jchar*chars=e->GetStringChars(value,nullptr);if(!chars)return false;NSString*text=[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(chars) length:length];e->ReleaseStringChars(value,chars);NSData*bytes=[text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];if(!bytes||memchr(bytes.bytes,0,bytes.length)){fail(e,"Native window title is not valid UTF-8 text");return false;}out.assign(static_cast<const char*>(bytes.bytes),bytes.length);return true;}
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_WindowSession_configureIdentity(JNIEnv*e,jclass,jstring label,jbyteArray png,jboolean headless){@autoreleasepool{
 if(e->ExceptionCheck())return;if(!pthread_main_np()||current||processVideo){fail(e,"Application identity requires the native window owner before initialization");return;}
 std::string name;if(!title(e,label,name))return;
 NSData*applicationIcon=nil;
 if(png){jsize length=e->GetArrayLength(png);if(length>0&&length<=8*1024*1024){NSMutableData*data=[NSMutableData dataWithLength:length];e->GetByteArrayRegion(png,0,length,static_cast<jbyte*>(data.mutableBytes));if(!e->ExceptionCheck())applicationIcon=data;}}
 if(e->ExceptionCheck())return;
 if(!headless&&!klj_window_prepare(e))return;
 if(!kl_application_start(name.c_str(),applicationIcon.bytes,applicationIcon.length,headless)){fail(e,SDL_GetError());return;}
 processVideo=true;processHeadless=headless;
}}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_WindowSession_shutdownApplication(JNIEnv*e,jclass){@autoreleasepool{
 if(!pthread_main_np()||current){fail(e,"Application window services still have a window owner");return;}
 if(processVideo){kl_application_stop();processVideo=false;}processHeadless=false;
}}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_WindowSession_allocateNative(JNIEnv*e,jclass){
 if(e->ExceptionCheck())return 0;if(!pthread_main_np()||current||nextIdentity>INT64_MAX){fail(e,"Native window session cannot be allocated");return 0;}
 auto s=std::unique_ptr<Owner>(new(std::nothrow)Owner);if(!s){fail(e,"Native window session allocation failed");return 0;}s->env=e;s->identity=nextIdentity++;
 if(!kl_session_init(&s->state,&ops,s.get(),reinterpret_cast<uintptr_t>(e))){fail(e,"Native window session configuration failed");return 0;}s->callbacks=kl_host_callbacks_create(e);if(!s->callbacks)return 0;current=s.release();return (jlong)current->identity;
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_WindowSession_initializeNative(JNIEnv*e,jclass,jlong id,jstring label,jint w,jint h){@autoreleasepool{
 auto s=owns(e,id);if(!s||e->ExceptionCheck())return;if(!title(e,label,s->label))return;
 if(!klj_resolution_ratio(getenv("RESHADE_SPATIAL_METHOD"),getenv("RESHADE_RESOLUTION_MULTIPLIER"),&s->numerator,&s->denominator)){fail(e,"Unsupported Android resolution multiplier");return;}
 if(!kl_session_open(&s->state,reinterpret_cast<uintptr_t>(e),w,h))fail(e,"Native window session initialization failed");
}}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_bridge_WindowSession_closeNative(JNIEnv*e,jclass,jlong id){
 auto s=owns(e,id);if(!s)return false;jthrowable first=nullptr;collect(e,s->callbacks,first);
 int status=kl_session_close(&s->state,reinterpret_cast<uintptr_t>(e));collect(e,s->callbacks,first);
 if(status==1){current=nullptr;delete s;}else if(status==0){fail(e,"Native window session cleanup failed");collect(e,s->callbacks,first);}restore(e,first);return status==1;
}
extern "C" void* klj_session_native_content(JNIEnv*e){if(!pthread_main_np()||!current||current->env!=e||!current->window)return nullptr;NSWindow*window=(__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(current->window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);return (__bridge void*)window.contentView;}
extern "C" KLHostCallbacks* klj_session_callbacks(JNIEnv*e){auto s=owns(e);return s&&!e->ExceptionCheck()?s->callbacks:nullptr;}
extern "C" int klj_session_claim(JNIEnv*e,KLSessionBranch branch,int w,int h,SDL_Window**windowOut,SDL_MetalView*viewOut){auto s=owns(e);if(!s||e->ExceptionCheck()||!windowOut||!viewOut)return 0;if(w!=klj_resolution_dimension(s->state.width,s->numerator,s->denominator)||h!=klj_resolution_dimension(s->state.height,s->numerator,s->denominator)||!kl_session_claim(&s->state,reinterpret_cast<uintptr_t>(e),branch,s->state.width,s->state.height)){fail(e,"Native renderer cannot adopt this window session");return 0;}*windowOut=s->window;*viewOut=s->view;return 1;}
extern "C" int klj_session_bind_activity(JNIEnv*e,jobject activity){@autoreleasepool{
 auto s=owns(e);if(!s||e->ExceptionCheck())return 0;
 if(!activity||(s->state.state!=KL_SESSION_READY&&s->state.state!=KL_SESSION_CLAIMED)){fail(e,"Activity requires its live window session");return 0;}
 if(s->activity&&e->IsSameObject(s->activity,activity)&&s->lifecycle.object)return 1;
 auto owner=kl_host_bind_KLHost_android_app_Activity(e,s->callbacks,activity);
 auto lifecycle=kl_host_lifecycle_owner(e,s->callbacks,owner);if(e->ExceptionCheck()||!lifecycle.object)return 0;
 jobject nextLifecycle=e->NewGlobalRef(lifecycle.object);e->DeleteLocalRef(lifecycle.object);if(!nextLifecycle)return 0;
 jobject nextActivity=e->NewGlobalRef(activity);if(!nextActivity){e->DeleteGlobalRef(nextLifecycle);return 0;}
 bool replacing=s->activity!=nullptr;if(s->activity)e->DeleteGlobalRef(s->activity);if(s->lifecycle.object)e->DeleteGlobalRef(s->lifecycle.object);
 s->activity=nextActivity;s->lifecycle.object=nextLifecycle;if(replacing)return 1;
 s->backAction=[MLAndroidBackAction new];s->backAction.window=(__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(s->window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);
 if(!NSApp.mainMenu)NSApp.mainMenu=[[NSMenu alloc]initWithTitle:@""];
 s->backMenu=[[NSMenuItem alloc]initWithTitle:@"Navigate" action:nil keyEquivalent:@""];NSMenu*menu=[[NSMenu alloc]initWithTitle:@"Navigate"];
 NSMenuItem*item=[[NSMenuItem alloc]initWithTitle:@"Back" action:@selector(goBack:) keyEquivalent:@"["];item.keyEquivalentModifierMask=NSEventModifierFlagCommand;item.target=s->backAction;[menu addItem:item];s->backMenu.submenu=menu;[NSApp.mainMenu addItem:s->backMenu];
 return 1;
}}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_WindowSession_attachActivityNative(JNIEnv*e,jclass,jlong id,jobject activity){if(owns(e,id))klj_session_bind_activity(e,activity);}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_WindowSession_orientationNative(JNIEnv*e,jclass,jobject activity,jboolean landscape){@autoreleasepool{
 auto s=owns(e);if(!s||e->ExceptionCheck())return;
 if(!s->activity||!e->IsSameObject(s->activity,activity)){fail(e,"Orientation requires the attached Activity");return;}
 int width=0,height=0;if(!SDL_GetWindowSize(s->window,&width,&height)){fail(e,SDL_GetError());return;}
 if((width>=height)==bool(landscape))return;
 if(s->state.state!=KL_SESSION_READY&&s->state.state!=KL_SESSION_CLAIMED){
  fail(e,"Orientation requires a live window session");return;
 }
 if(!placeWindow(s,height,width)||!SDL_GetWindowSize(s->window,&width,&height)){fail(e,SDL_GetError());return;}
 if(s->state.state==KL_SESSION_READY){
  if(!kl_session_resize_ready(&s->state,reinterpret_cast<uintptr_t>(e),width,height)){fail(e,"Orientation drawable could not be committed");return;}
 }
 /* Adopted renderers handle the real SDL resize and publish only
    after their own replacement texture is committed. Never publish this request. */
}}
extern "C" int klj_session_stop_notifications(JNIEnv*e){auto s=owns(e);if(!s)return 0;jthrowable first=nullptr;collect(e,s->callbacks,first);int status=kl_session_stop(&s->state,reinterpret_cast<uintptr_t>(e));collect(e,s->callbacks,first);restore(e,first);return status;}
extern "C" int klj_session_begin_close(JNIEnv*e){auto s=owns(e);return s&&(!s->scheduler||kl_host_scheduler_begin_close(e,s->scheduler));}
extern "C" int klj_session_quiesce_producer(JNIEnv*e,KLHostTaskPump pump,void* context,int (*retire)(void*),void* producer){
 auto s=owns(e);if(!s)return 0;jthrowable first=nullptr;collect(e,s->callbacks,first);uint64_t deadline=SDL_GetTicksNS()+1000000000ull;int status=0;
 bool producerRetired=retire==nullptr;
 s->pumpContext=context;s->pump=pump;
 for(;;){
  // Surface destruction can enqueue the render-thread release. Keep its
  // original owner pump and display clock alive until that release completes.
  if(!producerRetired)producerRetired=retire(producer)!=0;
  status=producerRetired?kl_session_stop(&s->state,reinterpret_cast<uintptr_t>(e)):KL_SESSION_BUSY;
  collect(e,s->callbacks,first);if(status!=KL_SESSION_BUSY)break;
  if(SDL_GetTicksNS()>=deadline){fail(e,producerRetired?"Display callbacks did not retire within the window teardown deadline":"Graphics producer did not retire within the window teardown deadline");collect(e,s->callbacks,first);status=0;break;}
  if(pump){pump(e,context);collect(e,s->callbacks,first);}SDL_WaitEventTimeout(nullptr,2);
 }
 s->pumpContext=nullptr;s->pump=nullptr;restore(e,first);return status==1;
}
extern "C" int klj_session_quiesce(JNIEnv*e,KLHostTaskPump pump,void*context){return klj_session_quiesce_producer(e,pump,context,nullptr,nullptr);}
extern "C" int klj_session_release(JNIEnv*e,KLSessionBranch branch,int drained){auto s=owns(e);if(!s)return 0;jthrowable first=nullptr;collect(e,s->callbacks,first);int status=kl_session_release(&s->state,reinterpret_cast<uintptr_t>(e),branch,drained);collect(e,s->callbacks,first);if(status==0){fail(e,"Native producer has not released this window session");collect(e,s->callbacks,first);}restore(e,first);return status;}
struct RunnablePump {KLHostCallbacks*callbacks;KLHost_java_lang_Runnable receiver;};
static void runTasks(JNIEnv*e,void*context){auto task=static_cast<RunnablePump*>(context);kl_host_run(e,task->callbacks,task->receiver);}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_bridge_WindowSession_stopNative(JNIEnv*e,jclass,jlong id,jobject tasks){
 auto s=owns(e,id);if(!s||e->ExceptionCheck())return false;if(!tasks){fail(e,"Window owner task pump is unavailable");return false;}
 auto receiver=kl_host_bind_KLHost_java_lang_Runnable(e,s->callbacks,tasks);
 RunnablePump callback{s->callbacks,receiver};
 return receiver.object&&!e->ExceptionCheck()&&klj_session_quiesce(e,runTasks,&callback);
}
extern "C" int klj_session_source_size(JNIEnv*e,int ow,int oh,int*w,int*h){auto s=owns(e);if(!s||!w||!h||e->ExceptionCheck())return 0;*w=klj_resolution_dimension(ow,s->numerator,s->denominator);*h=klj_resolution_dimension(oh,s->numerator,s->denominator);if(!*w||!*h){fail(e,"Android output extent is invalid");return 0;}return 1;}
extern "C" int klj_session_output_size(JNIEnv*e,int*w,int*h){auto s=owns(e);if(!s||!w||!h||e->ExceptionCheck())return 0;*w=s->state.width;*h=s->state.height;return 1;}
extern "C" int klj_session_input(JNIEnv*e,float*x,float*y){auto s=owns(e);if(!s||!x||!y||e->ExceptionCheck())return 0;if(!kl_window_input_coordinates(s->window,klj_resolution_dimension(s->state.width,s->numerator,s->denominator),klj_resolution_dimension(s->state.height,s->numerator,s->denominator),x,y)){fail(e,"Android input window extent is unavailable");return 0;}return 1;}
extern "C" int klj_session_committed_output(JNIEnv*e,KLSessionBranch branch,int w,int h,int ow,int oh){auto s=owns(e);if(!s||e->ExceptionCheck())return 0;if(w!=klj_resolution_dimension(ow,s->numerator,s->denominator)||h!=klj_resolution_dimension(oh,s->numerator,s->denominator)||!kl_session_commit(&s->state,reinterpret_cast<uintptr_t>(e),branch,ow,oh)){fail(e,"Committed Android source/output publication failed");return 0;}return 1;}
extern "C" int klj_session_committed(JNIEnv*e,KLSessionBranch branch,int w,int h){auto s=owns(e);return s?klj_session_committed_output(e,branch,w,h,s->state.width,s->state.height):0;}
extern "C" int klj_session_display_changed(JNIEnv*e){auto s=owns(e);if(!s||e->ExceptionCheck()||s->state.state!=KL_SESSION_CLAIMED)return 0;void*cocoa=SDL_GetPointerProperty(SDL_GetWindowProperties(s->window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);if(!kl_host_scheduler_changed_display(e,s->scheduler,cocoa))return 0;return bind(s,s->state.width,s->state.height);}
extern "C" int klj_session_pump(JNIEnv*e){auto s=owns(e);if(!s||e->ExceptionCheck())return 0;if(s->backAction.pending){s->backAction.pending=NO;kl_host_back(e,s->callbacks,s->lifecycle);if(e->ExceptionCheck())return 0;}return kl_host_scheduler_pump(e,s->scheduler);}
extern "C" int klj_session_event(JNIEnv*e,const SDL_Event*event){auto s=owns(e);return s?kl_host_scheduler_event(s->scheduler,event):0;}
extern "C" int klj_session_wait(JNIEnv*e,int existing){auto s=owns(e);return s&&!e->ExceptionCheck()?kl_host_scheduler_wait(e,s->scheduler,existing):-1;}

extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostTextInput_enableNative(JNIEnv*e,jclass,jboolean enabled){
 if(!pthread_main_np()||!current||current->env!=e||!current->window)return;
 kl_window_input_text_mode(current->window,enabled);
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostTextInput_areaNative(JNIEnv*e,jclass,jfloat left,jfloat top,jfloat right,jfloat bottom){
 if(!pthread_main_np()||!current||current->env!=e||!current->window)return;
 auto s=current;kl_window_physical_text_area(s->window,klj_resolution_dimension(s->state.width,s->numerator,s->denominator),klj_resolution_dimension(s->state.height,s->numerator,s->denominator),left,top,right,bottom);
}
extern "C" int klj_session_text_event(JNIEnv*e,const SDL_Event*event){
 if(!current||current->env!=e||!current->window||!event)return 0;auto s=current;SDL_WindowID id=SDL_GetWindowID(s->window);
 bool active=SDL_TextInputActive(s->window);
 if((event->type==SDL_EVENT_KEY_DOWN||event->type==SDL_EVENT_KEY_UP)&&event->key.windowID==id){bool requested=kl_host_text_active(e,s->callbacks);if(e->ExceptionCheck())return 1;if(requested!=active)kl_window_input_text_mode(s->window,requested);active=requested;}
 KLWindowEvent decoded;kl_window_input_decode(event,id,active,&decoded);
 if(decoded.value.kind==KL_INPUT_TEXT){
  bool composing=decoded.value.action;const char*bytes=decoded.text;
  if(bytes){size_t length=0;/* conversion below retains supplementary Unicode characters */
   char*utf16=SDL_iconv_string("UTF-16LE","UTF-8",bytes,strlen(bytes)+1);if(utf16){const uint16_t*characters=reinterpret_cast<const uint16_t*>(utf16);while(characters[length])length++;
    jstring text=e->NewString(reinterpret_cast<const jchar*>(characters),(jsize)length);SDL_free(utf16);if(text){kl_host_text_input(e,s->callbacks,{text},composing,decoded.start,decoded.length);e->DeleteLocalRef(text);}}
  }return 1;
 }
 if(decoded.value.kind==KL_INPUT_SHORTCUT){kl_host_text_shortcut(e,s->callbacks,decoded.value.code);return 1;}
 return decoded.value.kind==KL_INPUT_CONSUMED;
}
