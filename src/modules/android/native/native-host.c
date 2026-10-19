#include <jni.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "presentation.h"
#include "window.h"
#include "window-session.h"
#include "xr-presentation.h"
#include "window-input.h"
#include "application-sleep.h"
#include "native-gamepad.h"
#include "host-callbacks.h"
extern int klj_native_activity_pump(JNIEnv*);
extern void klj_native_egl_report(FILE*);
#ifdef KLJ_NATIVE_OVERLAY
#include "overlay.h"
static void *overlayOwner;
static JNIEnv *overlayEnv;
static uint64_t overlayHandle,nextOverlayHandle=1;
#endif
#if defined(KLJ_DECOR_TOUCH) && !defined(KLJ_NATIVE_OVERLAY)
#error Decor touch routing requires the native overlay hierarchy owner
#endif
extern void klj_surface_host(void *presenter);
extern bool klj_surface_detach_presenter(void *presenter);
static int retire_presenter(void *presenter){return klj_surface_detach_presenter(presenter);}
static int hostRunning;
static void fail(JNIEnv *env,const char *message){if((*env)->ExceptionCheck(env))return;jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");if(type)(*env)->ThrowNew(env,type,message);}
static void report_pending(JNIEnv *env,KLHostCallbacks *callbacks){
 if(!callbacks)return;
 jthrowable failure=(*env)->ExceptionOccurred(env);if(!failure)return;
 (*env)->ExceptionClear(env);
 kl_host_failure_report(env,callbacks,kl_host_bind_KLHost_java_lang_Throwable(env,callbacks,failure));
 // Diagnostics cannot replace the original exception. Guest renderer teardown
 // may terminate this dedicated process, so report before entering that code.
 if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
 (*env)->Throw(env,failure);(*env)->DeleteLocalRef(env,failure);
}
static void collect_failure(JNIEnv *env,KLHostCallbacks *callbacks,jthrowable *failure){
 jthrowable next=(*env)->ExceptionOccurred(env);if(!next)return;(*env)->ExceptionClear(env);
 if(!*failure){*failure=next;return;}
 if(!(*env)->IsSameObject(env,*failure,next)){
  kl_host_suppressed(env,callbacks,kl_host_bind_KLHost_java_lang_Throwable(env,callbacks,*failure),kl_host_bind_KLHost_java_lang_Throwable(env,callbacks,next));
  if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
 }
 (*env)->DeleteLocalRef(env,next);
}
typedef struct {KLHostCallbacks *callbacks;KLHost_maclauncher_android_framework_HostActivity lifecycle;} HostDrain;
static void drain_host(JNIEnv *env,void *context){HostDrain *drain=context;kl_host_drain(env,drain->callbacks,drain->lifecycle);}
static SDL_SystemCursor cursor_type(int type){
 switch(type){
  case 1002: return SDL_SYSTEM_CURSOR_POINTER;
  case 1004: return SDL_SYSTEM_CURSOR_WAIT;
  case 1007: return SDL_SYSTEM_CURSOR_CROSSHAIR;
  case 1008: return SDL_SYSTEM_CURSOR_TEXT;
  case 1012: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
  case 1013: return SDL_SYSTEM_CURSOR_MOVE;
  case 1014: return SDL_SYSTEM_CURSOR_EW_RESIZE;
  case 1015: return SDL_SYSTEM_CURSOR_NS_RESIZE;
  case 1016: return SDL_SYSTEM_CURSOR_NESW_RESIZE;
  case 1017: return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
  default: return SDL_SYSTEM_CURSOR_DEFAULT;
 }
}
static void run_host(JNIEnv *env,jobject view,jobject activity,jstring title,jint width,jint height){
 if(!view||!activity||!title||width<1||height<1||width>8192||height>8192||!pthread_main_np()||hostRunning){fail(env,"Native Android window configuration or ownership is invalid");return;}
 const char *label=(*env)->GetStringUTFChars(env,title,NULL);if(!label)return;
 hostRunning=1;
 SDL_Window *window=NULL;SDL_MetalView metalView=NULL;void *presenter=NULL,*projection=NULL;int claimed=0;
 KLJGamepads *gamepads=NULL;
 int outputWidth=0,outputHeight=0;
 const char *traceOption=getenv("KL_EGL_TRACE");int traceEgl=traceOption&&*traceOption&&strcmp(traceOption,"0");uint64_t nextEglReport=0;
 SDL_Cursor *cursor=NULL;int pointerType=-2,pointerCaptured=0,customCursors=0;
 jobject cursorRequest=NULL;
 #ifdef KLJ_NATIVE_OVERLAY
 KLHost_maclauncher_android_bridge_CanvasFrameSource overlaySource={0};uint64_t nextOverlayFrame=0;
 #endif
 jlong titleRevision=0; // Preserve the configured label until the Activity changes it.
 KLHostCallbacks *callbacks=NULL;
 KLHost_android_view_View viewOwner={0};
 KLHost_android_app_Activity activityOwner={0};
 KLHost_maclauncher_android_framework_HostActivity lifecycle={0};
 HostDrain drain={NULL,{0}};
 if(!(callbacks=klj_session_callbacks(env)))goto done;
 viewOwner=kl_host_bind_KLHost_android_view_View(env,callbacks,view);
 activityOwner=kl_host_bind_KLHost_android_app_Activity(env,callbacks,activity);
 lifecycle=kl_host_lifecycle_owner(env,callbacks,activityOwner);
 drain.callbacks=callbacks;drain.lifecycle=lifecycle;
 if((*env)->ExceptionCheck(env))goto done;
 if(!klj_session_output_size(env,&outputWidth,&outputHeight))goto done;
 // Adopt committed backing pixels after Retina conversion and screen fitting.
 if(!klj_session_source_size(env,outputWidth,outputHeight,&width,&height))goto done;
 if(!klj_session_claim(env,KL_SESSION_NATIVE,width,height,&window,&metalView))goto done;
 claimed=1;
 gamepads=klj_gamepads_create(env,window);if(!gamepads)goto done;
 if(!klj_session_bind_activity(env,activity))goto done;
 {presenter=klj_metal_create(env,SDL_Metal_GetLayer(metalView),width,height);if(!presenter)goto done;if(!klj_metal_output_size(env,presenter,outputWidth,outputHeight))goto done;klj_surface_host(presenter);}
 #ifdef KLJ_NATIVE_OVERLAY
 {
  if(overlayOwner||nextOverlayHandle>INT64_MAX){fail(env,"Native Android overlay owner is unavailable");goto done;}
  if(klj_metal_overlay_attach(env,presenter,width,height)!=0){fail(env,"Native Android overlay attachment failed");goto done;}
  overlayOwner=presenter;overlayEnv=env;overlayHandle=nextOverlayHandle++;
  overlaySource=kl_host_overlay_create(env,callbacks,activityOwner,(jlong)overlayHandle,width,height);
  if((*env)->ExceptionCheck(env))goto done;
  if(!overlaySource.object){fail(env,"Native Android overlay source was not created");goto done;}
 }
 #endif
 {projection=klj_xr_attach(env,presenter,SDL_Metal_GetLayer(metalView));if(!projection)goto done;}
 kl_host_resume(env,callbacks,lifecycle);if((*env)->ExceptionCheck(env))goto done;SDL_RaiseWindow(window);
 kl_host_focus(env,callbacks,lifecycle,(jboolean)((SDL_GetWindowFlags(window)&SDL_WINDOW_INPUT_FOCUS)!=0));if((*env)->ExceptionCheck(env))goto done;
 {SDL_Event focus={0};focus.type=(SDL_GetWindowFlags(window)&SDL_WINDOW_INPUT_FOCUS)?SDL_EVENT_WINDOW_FOCUS_GAINED:SDL_EVENT_WINDOW_FOCUS_LOST;focus.window.windowID=SDL_GetWindowID(window);if(klj_gamepads_event(env,gamepads,&focus)<0)goto done;}
 for(;;){
  if(!klj_session_pump(env))goto done;
  if(traceEgl&&SDL_GetTicksNS()>=nextEglReport){klj_native_egl_report(stderr);nextEglReport=SDL_GetTicksNS()+2000000000ull;}
  {uint64_t extent=klj_metal_committed_extent(presenter);if(extent&&(int)(extent>>32)==width&&(int)(extent&UINT32_MAX)==height&&!klj_session_committed_output(env,KL_SESSION_NATIVE,width,height,outputWidth,outputHeight))goto done;}
  kl_host_drain(env,callbacks,lifecycle);if((*env)->ExceptionCheck(env))goto done;
  if(!klj_native_activity_pump(env))goto done;
  int dialogModal=0;
  jlong version=kl_host_title_revision(env,callbacks,lifecycle);if((*env)->ExceptionCheck(env))goto done;
  if(version!=titleRevision){
   // The Activity stores an immutable String. Convert actual UTF-16 to UTF-8;
   // JNI's modified UTF-8 is not the encoding accepted by SDL window titles.
   jstring text=(jstring)kl_host_title(env,callbacks,lifecycle).object;if((*env)->ExceptionCheck(env))goto done;
   if(text){jsize length=(*env)->GetStringLength(env,text);if(length>16384){(*env)->DeleteLocalRef(env,text);fail(env,"Window title exceeds supported length");goto done;}
    jchar *chars=SDL_calloc((size_t)length+1,sizeof(jchar));if(!chars){(*env)->DeleteLocalRef(env,text);fail(env,"Window title allocation failed");goto done;}
    (*env)->GetStringRegion(env,text,0,length,chars);(*env)->DeleteLocalRef(env,text);if((*env)->ExceptionCheck(env)){SDL_free(chars);goto done;}
    char *utf8=SDL_iconv_string("UTF-8","UTF-16LE",(const char*)chars,((size_t)length+1)*2);SDL_free(chars);
    if(!utf8){fail(env,"Window title conversion failed");goto done;}
    int okay=SDL_SetWindowTitle(window,utf8);SDL_free(utf8);if(!okay){fail(env,SDL_GetError());goto done;}
   }
   titleRevision=version;
  }
  KLHost_android_view_PointerIcon requestedCursor=kl_host_pointer(env,callbacks,viewOwner);if((*env)->ExceptionCheck(env))goto done;
  if(pointerType==-2||!(*env)->IsSameObject(env,cursorRequest,requestedCursor.object)){
   int requestedPointer=requestedCursor.object?kl_host_pointer_kind(env,callbacks,requestedCursor):1000;if((*env)->ExceptionCheck(env))goto done;
   if(requestedPointer==0){if(!SDL_HideCursor()){fail(env,SDL_GetError());goto done;}}
   else{
    SDL_Cursor *next=NULL;
    if(requestedPointer==-1){
     jintArray packed=kl_host_pointer_pixels(env,callbacks,requestedCursor);if((*env)->ExceptionCheck(env)||!packed)goto done;
     jsize count=(*env)->GetArrayLength(env,packed);jint dimensions[4]={0};if(count<4){fail(env,"Custom cursor data is incomplete");goto done;}
     (*env)->GetIntArrayRegion(env,packed,0,4,dimensions);if((*env)->ExceptionCheck(env))goto done;
     int w=dimensions[0],h=dimensions[1],hx=dimensions[2],hy=dimensions[3];
     if(w<1||h<1||w>1024||h>1024||count!=4+w*h||hx<0||hy<0||hx>=w||hy>=h){fail(env,"Custom cursor data is invalid");goto done;}
     jint *pixels=(*env)->GetIntArrayElements(env,packed,NULL);if(!pixels)goto done;
     SDL_Surface *surface=SDL_CreateSurfaceFrom(w,h,SDL_PIXELFORMAT_ARGB8888,pixels+4,w*4);
     if(surface){next=SDL_CreateColorCursor(surface,hx,hy);SDL_DestroySurface(surface);}
     (*env)->ReleaseIntArrayElements(env,packed,pixels,JNI_ABORT);(*env)->DeleteLocalRef(env,packed);
     if(next)customCursors++;
    }else next=SDL_CreateSystemCursor(cursor_type(requestedPointer));
    if(!next){fail(env,SDL_GetError());goto done;}
    if(!SDL_SetCursor(next)||!SDL_ShowCursor()){SDL_DestroyCursor(next);fail(env,SDL_GetError());goto done;}
    SDL_DestroyCursor(cursor);cursor=next;
   }
   if(cursorRequest)(*env)->DeleteLocalRef(env,cursorRequest);cursorRequest=requestedCursor.object;requestedCursor.object=NULL;
   pointerType=requestedPointer;
  }
  if(requestedCursor.object)(*env)->DeleteLocalRef(env,requestedCursor.object);
  jboolean requestedCapture=kl_host_capture_requested(env,callbacks,viewOwner);if((*env)->ExceptionCheck(env))goto done;
  int actualCapture=requestedCapture&&!dialogModal&&SDL_GetKeyboardFocus()==window;
  if(actualCapture!=pointerCaptured){
   if(!SDL_SetWindowRelativeMouseMode(window,actualCapture)){fail(env,SDL_GetError());goto done;}
   pointerCaptured=actualCapture;
   kl_host_capture_changed(env,callbacks,viewOwner,(jboolean)actualCapture);if((*env)->ExceptionCheck(env))goto done;
  }
  jboolean requested=kl_host_closing(env,callbacks,lifecycle);if((*env)->ExceptionCheck(env))goto done;if(requested)break;
  SDL_Event event;int quit=0,awoke=0;while(SDL_PollEvent(&event)){
   int appInputHandled=0;
   int sleeping=0;if(kl_application_sleep_event(&event,SDL_GetWindowID(window),&sleeping)){kl_host_sleep(env,callbacks,(jboolean)sleeping);if((*env)->ExceptionCheck(env))goto done;continue;}
   if(klj_session_event(env,&event)){awoke=1;continue;}
   int gamepadEvent=event.type==SDL_EVENT_WINDOW_FOCUS_GAINED?0:klj_gamepads_event(env,gamepads,&event);if(gamepadEvent<0)goto done;if(gamepadEvent)continue;
   unsigned owned=SDL_GetWindowID(window);
   KLWindowEvent decoded;kl_window_input_decode(&event,owned,0,&decoded);
   KLWindowInput *physical=&decoded.value;
   if(physical->kind==KL_INPUT_CLOSE){
    kl_host_window_close_requested(env,callbacks);if((*env)->ExceptionCheck(env))goto done;quit=1;
   }
   if(event.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED&&event.window.windowID==owned){if(!klj_session_display_changed(env))goto done;}
   if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED&&event.window.windowID==owned){
    if(event.window.data1<1||event.window.data2<1)continue;
    int sourceWidth,sourceHeight;if(!klj_session_source_size(env,event.window.data1,event.window.data2,&sourceWidth,&sourceHeight))goto done;
    if(!klj_metal_output_size(env,presenter,event.window.data1,event.window.data2))goto done;
    outputWidth=event.window.data1;outputHeight=event.window.data2;width=sourceWidth;height=sourceHeight;
    #ifdef KLJ_NATIVE_OVERLAY
    if(overlaySource.object){kl_host_frame_resize(env,callbacks,overlaySource,width,height);if((*env)->ExceptionCheck(env))goto done;}
    #endif
    /* Original ViewRoot/SurfaceView publishes size changes to its actual
       producer. Its render thread recreates EGL storage; the next committed
       extent publishes display metrics, never this window-size request. */
   }
   if(physical->kind==KL_INPUT_FOCUS){
    kl_host_focus(env,callbacks,lifecycle,(jboolean)physical->action);
    if((*env)->ExceptionCheck(env))goto done;
    if(event.type==SDL_EVENT_WINDOW_FOCUS_GAINED&&klj_gamepads_event(env,gamepads,&event)<0)goto done;
   }
   if(physical->kind==KL_INPUT_WHEEL){
    float x=physical->x,y=physical->y;if(!klj_session_input(env,&x,&y))goto done;
    // SDL already applies the user's natural-scroll preference. Keep its signed
    // axes, including native trackpad momentum, at the Android input boundary.
    kl_host_wheel(env,callbacks,x,y,physical->dx,physical->dy,(jboolean)!pointerCaptured);if((*env)->ExceptionCheck(env))goto done;continue;
   }
   if(pointerCaptured&&((event.type==SDL_EVENT_MOUSE_MOTION&&event.motion.windowID==owned)||((event.type==SDL_EVENT_MOUSE_BUTTON_DOWN||event.type==SDL_EVENT_MOUSE_BUTTON_UP)&&event.button.windowID==owned))){
    int action=event.type==SDL_EVENT_MOUSE_MOTION?2:event.type==SDL_EVENT_MOUSE_BUTTON_DOWN?11:12;
    float dx=event.type==SDL_EVENT_MOUSE_MOTION?event.motion.xrel:0,dy=event.type==SDL_EVENT_MOUSE_MOTION?event.motion.yrel:0;
    if(!klj_session_input(env,&dx,&dy))goto done;
    SDL_MouseButtonFlags held=SDL_GetMouseState(NULL,NULL);int buttons=(held&SDL_BUTTON_LMASK?1:0)|(held&SDL_BUTTON_RMASK?2:0)|(held&SDL_BUTTON_MMASK?4:0);
    jobject input=kl_host_relative(env,callbacks,action,dx,dy,buttons).object;if(!input||(*env)->ExceptionCheck(env))goto done;
    kl_host_input(env,callbacks,kl_host_bind_KLHost_android_view_InputEvent(env,callbacks,input));(*env)->DeleteLocalRef(env,input);if((*env)->ExceptionCheck(env))goto done;continue;
   }
   if(physical->kind==KL_INPUT_TOUCH){float x=physical->x,y=physical->y;if(!klj_session_input(env,&x,&y))goto done;jobject input=kl_host_motion(env,callbacks,physical->action,x,y).object;if((*env)->ExceptionCheck(env)||!input)goto done;
    appInputHandled=kl_host_input(env,callbacks,kl_host_bind_KLHost_android_view_InputEvent(env,callbacks,input));(*env)->DeleteLocalRef(env,input);if((*env)->ExceptionCheck(env))goto done;
   }
   if(klj_session_text_event(env,&event)){if((*env)->ExceptionCheck(env))goto done;continue;}
   if(physical->kind==KL_INPUT_KEY){
    jobject input=kl_host_key(env,callbacks,physical->action,physical->code,decoded.unicode,physical->meta).object;if((*env)->ExceptionCheck(env)||!input)goto done;appInputHandled=kl_host_input(env,callbacks,kl_host_bind_KLHost_android_view_InputEvent(env,callbacks,input));(*env)->DeleteLocalRef(env,input);if((*env)->ExceptionCheck(env))goto done;
   }
   if(projection)klj_xr_event(projection,window,&event,appInputHandled);
  }
  if(quit)break;
  if(projection){klj_xr_controls(projection,window,!dialogModal);if(!klj_xr_pump(env,projection,width,height))goto done;}
  #ifdef KLJ_NATIVE_OVERLAY
  if(overlaySource.object){uint64_t now=SDL_GetTicksNS();if(now>=nextOverlayFrame){
   kl_host_frame_pump(env,callbacks,overlaySource);if((*env)->ExceptionCheck(env))goto done;
   nextOverlayFrame=nextOverlayFrame?nextOverlayFrame+16666667:now+16666667;
   if(nextOverlayFrame<=SDL_GetTicksNS())nextOverlayFrame=SDL_GetTicksNS()+16666667;
  }}
  #endif
  uint64_t uiDeadline=0;
  #ifdef KLJ_NATIVE_OVERLAY
  if(overlaySource.object&&(!uiDeadline||nextOverlayFrame<uiDeadline))uiDeadline=nextOverlayFrame;
  #endif
  int waitMillis=2;
  if(uiDeadline){
   // A null event stays queued. SDL input wakes immediately; Java-only tasks
   // are serviced on the next at-most17ms owner iteration.
   uint64_t now=SDL_GetTicksNS();waitMillis=0;if(uiDeadline>now){uint64_t millis=(uiDeadline-now+999999)/1000000;waitMillis=(int)(millis>17?17:millis);}
  }
  waitMillis=klj_session_wait(env,awoke?0:waitMillis);if(waitMillis<0)goto done;
  if(waitMillis)SDL_WaitEventTimeout(NULL,waitMillis);
 }
done:;
 klj_native_egl_report(stderr);
 report_pending(env,callbacks);
 jthrowable failure=NULL;collect_failure(env,callbacks,&failure);
 // Android pauses/stops the Activity while its window, display callbacks and
 // render thread are still live. Engines can wait for a final paced frame in
 // onPause and surfaceDestroyed; stopping the clock first deadlocks that handoff.
 if(claimed){klj_session_begin_close(env);collect_failure(env,callbacks,&failure);}
 if(gamepads){klj_gamepads_close(env,gamepads);gamepads=NULL;collect_failure(env,callbacks,&failure);}
 if(window&&lifecycle.object){kl_host_focus(env,callbacks,lifecycle,(jboolean)false);collect_failure(env,callbacks,&failure);}
 if(window&&pointerCaptured){if(!SDL_SetWindowRelativeMouseMode(window,false))fail(env,SDL_GetError());collect_failure(env,callbacks,&failure);pointerCaptured=0;}
 if(viewOwner.object){kl_host_capture_release(env,callbacks,viewOwner);collect_failure(env,callbacks,&failure);}
 if(viewOwner.object){kl_host_capture_changed(env,callbacks,viewOwner,(jboolean)false);collect_failure(env,callbacks,&failure);}
 if(lifecycle.object){kl_host_finish(env,callbacks,lifecycle);collect_failure(env,callbacks,&failure);}
 // The Android window service owns compositor buffers in addition to Activity
 // surfaces. Release that graph before its frame source, native presenter or
 // display callbacks: outstanding references are not a second live window.
 if(callbacks){kl_host_finish_presentation(env,callbacks);collect_failure(env,callbacks,&failure);}
 #ifdef KLJ_NATIVE_OVERLAY
 if(overlaySource.object){kl_host_frame_close(env,callbacks,overlaySource);collect_failure(env,callbacks,&failure);}
 #endif
 if(failure){(*env)->Throw(env,failure);(*env)->DeleteLocalRef(env,failure);failure=NULL;}
 if(projection&&!klj_xr_retire(projection)){(*env)->ReleaseStringUTFChars(env,title,label);return;}
 if(claimed&&!klj_session_quiesce_producer(env,lifecycle.object?drain_host:NULL,&drain,retire_presenter,presenter)){
  // Failed retirement retains the producer/presenter/window until process exit.
  (*env)->ReleaseStringUTFChars(env,title,label);return;
 }
 collect_failure(env,callbacks,&failure);
 klj_surface_host(NULL);
 #ifdef KLJ_NATIVE_OVERLAY
 overlayOwner=NULL;overlayEnv=NULL;overlayHandle=0;
 #endif
 if(failure){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,failure);(*env)->DeleteLocalRef(env,failure);}
 if(!klj_surface_detach_presenter(presenter)){
  // An active producer or enqueue still needs the display. Ordinary retained
  // buffer references have already detached and keep only their IOSurfaces.
  fprintf(stderr,"[android-surface] A producer remains active at shutdown; its presenter retained until process exit\n");
 }else{
  klj_metal_destroy(presenter);
  if(projection)klj_xr_detach(projection);
  if(cursor){SDL_SetCursor(NULL);SDL_DestroyCursor(cursor);}SDL_ShowCursor();
  if(claimed)klj_session_release(env,KL_SESSION_NATIVE,1);
 }
 #ifdef KLJ_NATIVE_OVERLAY
 if(overlaySource.object)(*env)->DeleteLocalRef(env,overlaySource.object);
 #endif
 if(cursorRequest)(*env)->DeleteLocalRef(env,cursorRequest);
 if(customCursors)fprintf(stderr,"[native-surface] applied %d custom cursor requests\n",customCursors);
 
 (*env)->ReleaseStringUTFChars(env,title,label);
 hostRunning=0;
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_run(JNIEnv *env,jclass ignored,jobject view,jobject activity,jstring title,jint width,jint height){run_host(env,view,activity,title,width,height);}
#ifdef KLJ_NATIVE_OVERLAY
static void *overlay_owner(JNIEnv *env,jlong handle){if(!overlayOwner||handle<=0||(uint64_t)handle!=overlayHandle||env!=overlayEnv||!SDL_IsMainThread()){fail(env,"Android overlay no longer owns this host window");return NULL;}return overlayOwner;}
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_CanvasOverlay_submitNative(JNIEnv *env,jclass ignored,jlong handle,jintArray pixels,jint width,jint height,jlong frameId){void *owner=overlay_owner(env,handle);if(!owner)return 4;if(frameId<=0){fail(env,"Android overlay frame identity is invalid");return 4;}return klj_metal_overlay_submit(env,owner,pixels,0,width,width,height,(uint64_t)frameId);}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_CanvasOverlay_validateNative(JNIEnv *env,jclass ignored,jlong handle,jint width,jint height){void *owner=overlay_owner(env,handle);if(owner&&klj_metal_overlay_validate_size(env,owner,width,height)!=0)fail(env,"Android overlay dimensions are unavailable");}
#endif
