#include "probe.h"
#include <android/configuration.h>
#include <android/hardware_buffer.h>
#include <android/bitmap.h>
#include <android/data_space.h>
#include <android/looper.h>
#include <EGL/egl.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
typedef struct {
 NativeThreadCall *call;
 ALooper *looper;
 pthread_t physical;
 pid_t tid;
 int descriptor,keep,expected;
} Callback;

static int detached(JavaVM *vm) {
 void *environment=(void *)(uintptr_t)1;
 return (*vm)->GetEnv(vm,&environment,JNI_VERSION_1_6)==JNI_EDETACHED && !environment;
}

typedef struct {NativeThreadCall *call;unsigned count;size_t used;unsigned char bytes[1024];} Png;
static bool compressed(void *raw,const void *data,size_t size) {
 Png *png=raw;
 if(!detached(png->call->vm)||size>sizeof png->bytes-png->used)return false;
 memcpy(png->bytes+png->used,data,size);png->used+=size;png->count++;return true;
}

static int pure_c(NativeThreadCall *call) {
 CHECK(detached(call->vm));
 if(call->groups&PROBE_NDK) {
  AConfiguration *configuration=AConfiguration_new();
  CHECK(configuration);
  AConfiguration_setDensity(configuration,321);
  CHECK(AConfiguration_getDensity(configuration)==321);
  AConfiguration_delete(configuration);
 }
 if(call->groups&PROBE_GRAPHICS) {
  const unsigned char pixels[]={255,0,0,255,0,255,0,255};
  AndroidBitmapInfo info={.width=2,.height=1,.stride=8,.format=ANDROID_BITMAP_FORMAT_RGBA_8888,.flags=ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE};
  Png png={.call=call};
  CHECK(AndroidBitmap_compress(&info,ADATASPACE_SRGB,pixels,ANDROID_BITMAP_COMPRESS_FORMAT_PNG,100,&png,compressed)==ANDROID_BITMAP_RESULT_SUCCESS);
  const unsigned char signature[]={137,80,78,71,13,10,26,10};
  CHECK(png.count&&png.used>24&&!memcmp(png.bytes,signature,sizeof signature)&&png.bytes[19]==2&&png.bytes[23]==1);
 }
 if(call->groups&PROBE_EGL) {
  CHECK(eglGetError()==EGL_SUCCESS);
  CHECK(eglGetCurrentContext()==EGL_NO_CONTEXT);
  CHECK(eglGetCurrentDisplay()==EGL_NO_DISPLAY);
  CHECK(eglGetCurrentSurface(EGL_DRAW)==EGL_NO_SURFACE);
  EGLint value=123;
  CHECK(!eglQueryContext(EGL_NO_DISPLAY,EGL_NO_CONTEXT,EGL_CONTEXT_CLIENT_TYPE,&value));
  CHECK(eglGetError()==EGL_BAD_DISPLAY && eglGetError()==EGL_SUCCESS);
  CHECK(eglGetProcAddress("eglGetError")==
    (__eglMustCastToProperFunctionPointerType)eglGetError);
 }
 CHECK(detached(call->vm));
 return 0;
}

static int ready_callback(int descriptor,int events,void *raw) {
 Callback *callback=raw;
 NativeThreadCall *call=callback->call;
 int line=0;
 if(descriptor!=callback->descriptor || events!=ALOOPER_EVENT_INPUT ||
    gettid()!=callback->tid || !pthread_equal(pthread_self(),callback->physical))
  line=__LINE__;
 if(!line && !detached(call->vm)) line=__LINE__;
 if(!line) call->inside++;
 if(!line && ALooper_forThread()!=callback->looper) line=__LINE__;
 char value=0;
 if(!line && (read(descriptor,&value,1)!=1 || value!=callback->expected)) line=__LINE__;
 if(!line) line=pure_c(call);
 if(line) { call->callback_line=line; return 0; }
 call->callbacks++;
 return callback->keep;
}

static int poll_callbacks(Callback *callback,int expected) {
 for(unsigned i=0;i<8 && callback->call->callbacks<expected;i++) {
  int result=ALooper_pollOnce(0,NULL,NULL,NULL);
  CHECK(result==ALOOPER_POLL_CALLBACK || result==ALOOPER_POLL_WAKE);
  CHECK(!callback->call->callback_line);
 }
 CHECK(callback->call->callbacks==expected);
 return 0;
}

static int exercise_worker(NativeThreadCall *call) {
 call->worker_tid=gettid();call->worker_pthread=(uintptr_t)pthread_self();
 CHECK(call->worker_tid>0 && call->worker_tid!=call->owner_tid);
 CHECK(detached(call->vm));call->before++;
 CHECK(!pure_c(call));
 CHECK(!ALooper_forThread());
 ALooper *looper=ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
 CHECK(looper && ALooper_forThread()==looper);
 ALooper_acquire(looper);
 int pair[2];CHECK(pipe(pair)==0);
 Callback callback={call,looper,pthread_self(),gettid(),pair[0],1,71};
 CHECK(ALooper_addFd(looper,pair[0],-1,ALOOPER_EVENT_INPUT,ready_callback,&callback)==1);
 char value=71;CHECK(write(pair[1],&value,1)==1);CHECK(!poll_callbacks(&callback,1));
 CHECK(write(pair[1],&value,1)==1);CHECK(!poll_callbacks(&callback,2));
 CHECK(ALooper_removeFd(looper,pair[0])==1 && ALooper_removeFd(looper,pair[0])==0);
 callback.keep=0;
 CHECK(ALooper_addFd(looper,pair[0],-1,ALOOPER_EVENT_INPUT,ready_callback,&callback)==1);
 CHECK(write(pair[1],&value,1)==1);CHECK(!poll_callbacks(&callback,3));
 CHECK(ALooper_removeFd(looper,pair[0])==0);
 CHECK(close(pair[0])==0 && close(pair[1])==0);
 /* Keep one real descriptor lease for physical thread retirement. The callback
  * is never made ready, so its stack data cannot escape through an invocation. */
 CHECK(pipe(pair)==0);
 CHECK(ALooper_addFd(looper,pair[0],99,ALOOPER_EVENT_INPUT,NULL,NULL)==1);
 call->retired_read=pair[0];call->retired_write=pair[1];
 ALooper_release(looper);
 CHECK(detached(call->vm));call->after++;
 return 0;
}

static void *worker(void *raw) {
 NativeThreadCall *call=raw;
 call->line=exercise_worker(call);
 return NULL;
}

uint64_t native_thread_exercise(uint64_t raw) {
 NativeThreadCall *call=(void *)(uintptr_t)raw;
 CHECK(call && call->vm && call->groups<=PROBE_ALL);
 call->owner_tid=gettid();call->retired_read=call->retired_write=-1;
 pthread_t thread;CHECK(!pthread_create(&thread,NULL,worker,call));
 CHECK(!pthread_join(thread,NULL));
 CHECK(!call->line && !call->callback_line);
 CHECK(call->before==1 && call->inside==3 && call->after==1 && call->callbacks==3);
 CHECK(call->retired_read>=0 && call->retired_write>=0);
 CHECK(close(call->retired_read)==0 && close(call->retired_write)==0);
 return 0;
}

/* The image callback owner must reject this target with a pending throwable.
 * Direct runtime entry may retain its existing C semantics. Both routes carry
 * the same genuine executable ELF PC and preserve the throwable identity. */
uint64_t native_thread_forbidden(uint64_t raw) {
 unsigned *count=(void *)(uintptr_t)raw;
 (*count)++;
 return 0;
}
