#include "probe.h"
#include "runtime-host.h"
#include "image-loader.h"
#include "native-image-provider.h"
#include "jni-provider.h"
#include "native-import-registry.h"
#include "looper-host.h"
#include "ndk-host.h"
#include "graphics-host.h"
#include "egl-host.h"
#include <android/hardware_buffer.h>
#include <android/bitmap.h>
#include <android/data_space.h>
#include <EGL/egl.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static KljBionicRuntime *runtime;
static JavaVM *real_vm;
static const KljBionicPollFdApi *poll_api;
static void *ndk;
static jlong image;
static unsigned groups;
static _Atomic unsigned borrowed,released,callbacks,native_calls;
static uint64_t native_physical;
extern void *kl_ndk_lookup(const char *);
extern void *klj_guest_image_symbol(jlong,const char *);

static uint64_t physical(void) {
 uint64_t identifier=0;assert(!pthread_threadid_np(NULL,&identifier)&&identifier);
 return identifier;
}
static void detached_host(void) {
 void *environment=(void *)(uintptr_t)1;
 assert((*real_vm)->GetEnv(real_vm,&environment,JNI_VERSION_1_6)==JNI_EDETACHED&&!environment);
 uint64_t identifier=physical();
 if(!native_physical)native_physical=identifier;
 assert(native_physical==identifier);
}
static int64_t borrow(int descriptor,int *native,void **lease) {
 int64_t status=poll_api->borrow(descriptor,native,lease);
 if(!status){assert(*native>=0&&*native!=descriptor&&*lease);borrowed++;}
 return status;
}
static int64_t release(void *lease) {
 int64_t status=poll_api->release(lease);assert(!status);released++;return status;
}
static const KljBionicPollFdApi counted={sizeof counted,1,borrow,release};
static void *resolve(const char *name) {
 void *value=ndk?dlsym(ndk,name):NULL;
 return value?value:klj_resolve_import(name);
}
static JNIEnv *environment(void *unused,JNIEnv *real,JNIEnv *outer) {
 (void)unused;
 if(!real||!outer)return NULL;
 return klj_bionic_jni_environment(klj_bionic_runtime_jni_owner(runtime),real)==outer?real:NULL;
}
static int64_t invoke(void *unused,JNIEnv *env,uintptr_t caller,KljCallPacket *packet) {
 (void)unused;assert(!env);detached_host();
 const void *identity=klj_jni_provider_identity((void *)caller);
 assert(identity&&(jlong)(uintptr_t)identity==image);
 /* This route checks live classloader reachability and the target's ELF PF_X
  * mapping before the runtime's same-thread nested host_scope re-entry. */
 int found=klj_native_image_invoke(NULL,(jlong)(uintptr_t)identity,packet);
 detached_host();callbacks++;
 return found?packet->status:-22;
}
static int64_t host_call(void *unused,KljBionicRuntime *owner,JNIEnv *env,
 uint32_t code,uint64_t raw,uint64_t length) {
 (void)unused;assert(owner==runtime);
 if(!env){detached_host();native_calls++;}
 if(length!=sizeof(KljBionicNdkPacket))return -22;
 KljBionicNdkPacket *packet=(void *)(uintptr_t)raw;
 switch(code) {
  case KLJ_BIONIC_LOOPER_HOSTCALL:return klj_bionic_looper_dispatch(env,packet);
  case KLJ_BIONIC_NDK_HOSTCALL:return klj_bionic_ndk_dispatch(env,packet);
  case KLJ_BIONIC_GRAPHICS_HOSTCALL:return klj_bionic_graphics_dispatch(env,packet);
  case KLJ_BIONIC_EGL_HOSTCALL:return klj_bionic_egl_dispatch(env,packet);
  default:return -38;
 }
}
static int64_t install(JNIEnv *env,const char *name,const KljBionicNdkBindings *bindings) {
 if(!bindings)return -22;
 KljBionicSymbol symbol={sizeof symbol,1,klj_bionic_runtime_guest(runtime)->helper.handle,name,NULL,NULL,0};
 int64_t status=klj_bionic_runtime_symbol(runtime,env,&symbol);uint64_t result=0;
 if(!status)status=klj_bionic_runtime_invoke(runtime,env,(uintptr_t)symbol.symbol,(uintptr_t)bindings,&result);
 return status?status:(int64_t)result;
}
static int prepare_egl(JNIEnv *env) {
 const char *graphics=getenv("EGL_GRAPHICS_LIBRARY"),*angle=getenv("EGL_ANGLE_ROOT");
 assert(graphics&&angle);
 jclass type=(*env)->FindClass(env,"maclauncher/android/bridge/Graphics");assert(type);
 jmethodID method=(*env)->GetStaticMethodID(env,type,"configureHost","(Ljava/lang/String;Ljava/lang/String;II)V");assert(method);
 jstring value=(*env)->NewStringUTF(env,angle);
 (*env)->CallStaticVoidMethod(env,type,method,NULL,value,1,1);
 (*env)->DeleteLocalRef(env,value);(*env)->DeleteLocalRef(env,type);assert(!(*env)->ExceptionCheck(env));
 type=(*env)->FindClass(env,"maclauncher/android/bridge/NativeSurface");assert(type);
 method=(*env)->GetStaticMethodID(env,type,"configureGraphics","(Ljava/lang/String;)V");assert(method);
 value=(*env)->NewStringUTF(env,graphics);(*env)->CallStaticVoidMethod(env,type,method,value);
 (*env)->DeleteLocalRef(env,value);(*env)->DeleteLocalRef(env,type);assert(!(*env)->ExceptionCheck(env));
 void *library=dlopen(graphics,RTLD_NOW|RTLD_NOLOAD);assert(library);
 void *lookup=dlsym(library,"klj_native_egl_lookup"),*error=dlsym(library,"klj_native_egl_error");assert(lookup&&error);
 KljBionicEglOwner owner;memcpy(&owner.resolve,&lookup,sizeof owner.resolve);memcpy(&owner.error,&error,sizeof owner.error);
 return klj_bionic_egl_prepare(&owner);
}
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_NativeThreadProbe_initialize(
 JNIEnv *env,jclass ignored,jobjectArray paths,jint enabled) {
 (void)ignored;assert(enabled>=0&&enabled<=PROBE_ALL);groups=(unsigned)enabled;
 assert((*env)->GetJavaVM(env,&real_vm)==JNI_OK);
 assert((*env)->GetArrayLength(env,paths)==7);const char *text[7];jstring strings[7];
 for(unsigned i=0;i<7;i++){strings[i]=(*env)->GetObjectArrayElement(env,paths,i);text[i]=(*env)->GetStringUTFChars(env,strings[i],NULL);assert(text[i]);}
 extern char **environ;size_t count=0;while(environ[count])count++;
 KljBionicRuntimeOptions options={sizeof options,1,text[0],text[1],text[2],text[3],text[4],text[5],text[6],(const char *const*)environ,count};
 int64_t status=klj_bionic_runtime_prepare(env,&options,&runtime);
 if(!status)status=klj_bionic_images_install(runtime,env);
 if(!status) {
  void *abi=dlopen(text[0],RTLD_NOW|RTLD_NOLOAD);assert(abi);
  const KljBionicPollFdApi *(*getter)(void)=dlsym(abi,"klj_bionic_poll_fd_api_v1");assert(getter);
  poll_api=getter();assert(poll_api&&poll_api->size==sizeof *poll_api&&poll_api->version==1);
  KljBionicLooperOwner owner={kl_ndk_lookup,invoke,NULL,&counted};
  status=klj_bionic_looper_prepare(env,&owner);
 }
 if(!status&&(groups&PROBE_GRAPHICS)) {
  const char *file=getenv("NDK_HOST_LIBRARY");assert(file);
  ndk=dlopen(file,RTLD_NOW|RTLD_LOCAL);assert(ndk);
 }
 if(!status&&(groups&PROBE_NDK)) {
  extern void Java_maclauncher_android_bridge_NativeBinding_prepareNativeActivity(JNIEnv*,jclass);
  Java_maclauncher_android_bridge_NativeBinding_prepareNativeActivity(env,ignored);
  if((*env)->ExceptionCheck(env))status=-22;
 }
 if(!status&&(groups&PROBE_NDK)) {
  KljBionicNdkOwner owner={.resolve=klj_resolve_import,.environment=environment};
  status=klj_bionic_ndk_prepare(&owner);
 }
 if(!status&&(groups&PROBE_GRAPHICS)) {
  KljBionicGraphicsOwner owner={resolve,environment,invoke,NULL};
  status=klj_bionic_graphics_prepare(&owner);
 }
 if(!status&&(groups&PROBE_EGL))status=prepare_egl(env);
 if(!status&&(groups&PROBE_NDK)) {
  struct {JNIEnv *env;jobject object;void *result;} arguments={(void*)1,NULL,(void*)1};
  KljBionicNdkPacket packet={sizeof packet,1,klj_bionic_ndk_bindings()->targets[3].target,(uintptr_t)&arguments,sizeof arguments,0,0,-1};
  assert(klj_bionic_ndk_dispatch(NULL,&packet)==-22&&arguments.result==(void*)1);
 }
 if(!status&&(groups&PROBE_GRAPHICS)) {
  struct {JNIEnv *env;jobject object;AndroidBitmapInfo *info;int result;} arguments={(void*)1,NULL,NULL,123};
  KljBionicNdkPacket packet={sizeof packet,1,klj_bionic_graphics_bindings()->targets[19].target,(uintptr_t)&arguments,sizeof arguments,0,0,-1};
  assert(klj_bionic_graphics_dispatch(NULL,&packet)==-22&&arguments.result==123);
 }
 if(!status)status=klj_bionic_runtime_host_callback(runtime,host_call,NULL);
 if(!status)status=install(env,"klj_bionic_looper_install",klj_bionic_looper_bindings());
 if(!status&&(groups&PROBE_NDK))status=install(env,"klj_bionic_ndk_install",klj_bionic_ndk_bindings());
 if(!status&&(groups&PROBE_GRAPHICS))status=install(env,"klj_bionic_graphics_install",klj_bionic_graphics_bindings());
 if(!status&&(groups&PROBE_EGL))status=install(env,"klj_bionic_egl_install",klj_bionic_egl_bindings());
 for(unsigned i=0;i<7;i++){(*env)->ReleaseStringUTFChars(env,strings[i],text[i]);(*env)->DeleteLocalRef(env,strings[i]);}
 fprintf(stderr,"Native pthread CPU control prepare=%lld groups=%u\n",(long long)status,groups);
 return (jint)status;
}

static void preserve_pending(JNIEnv *env,jthrowable expected) {
 jthrowable actual=(*env)->ExceptionOccurred(env);assert(actual);
 (*env)->ExceptionClear(env);assert((*env)->IsSameObject(env,expected,actual));
 (*env)->DeleteLocalRef(env,actual);assert(!(*env)->Throw(env,expected));
}
static void pending_dispatch(JNIEnv *env,jthrowable expected,
 const KljBionicNdkBindings *bindings,unsigned index,
 int64_t (*dispatch)(JNIEnv *,KljBionicNdkPacket *),void *arguments,size_t size) {
 assert(bindings&&index<bindings->count&&bindings->targets[index].argument_size==size);
 KljBionicNdkPacket packet={sizeof packet,1,bindings->targets[index].target,(uintptr_t)arguments,size,0,0,-1};
 assert(dispatch(env,&packet)==0);preserve_pending(env,expected);
}
static void pending_controls(JNIEnv *env,uintptr_t forbidden) {
 jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");assert(type);
 assert(!(*env)->ThrowNew(env,type,"native-thread pending sentinel"));
 jthrowable expected=(*env)->ExceptionOccurred(env);assert(expected);
 unsigned ran=0;uint64_t result=UINT64_C(0x12345678);
 int64_t status=klj_bionic_runtime_invoke(runtime,env,forbidden,(uintptr_t)&ran,&result);
 assert(status<=0);
 if(!status)assert(ran==1&&!result);
 else assert(!ran&&result==UINT64_C(0x12345678));
 preserve_pending(env,expected);unsigned original=ran;
 KljCallPacket packet={.size=sizeof packet,.version=KLJ_CALL_PACKET_VERSION,.target=forbidden,.x={(uintptr_t)&ran}};
 assert(klj_native_image_invoke(env,image,&packet)==1&&packet.status<0&&ran==original);
 preserve_pending(env,expected);
 struct{void *result;} pointer={(void *)(uintptr_t)1};
 pending_dispatch(env,expected,klj_bionic_looper_bindings(),2,klj_bionic_looper_dispatch,&pointer,sizeof pointer);
 assert(!pointer.result);
 if(groups&PROBE_NDK) {
  pending_dispatch(env,expected,klj_bionic_ndk_bindings(),46,klj_bionic_ndk_dispatch,&pointer,sizeof pointer);
  assert(pointer.result);
  struct {void *configuration;} arguments={pointer.result};
  pending_dispatch(env,expected,klj_bionic_ndk_bindings(),19,klj_bionic_ndk_dispatch,&arguments,sizeof arguments);
 }
 if(groups&PROBE_GRAPHICS) {
  AndroidBitmapInfo info={.width=2,.height=1,.stride=8,.format=ANDROID_BITMAP_FORMAT_RGBA_8888,.flags=ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE};
  const unsigned char pixels[]={255,0,0,255,0,255,0,255};
  struct {const AndroidBitmapInfo *info;int32_t space;const void *pixels;int32_t format,quality;void *user;AndroidBitmap_CompressWriteFunc callback;int result;} arguments={&info,ADATASPACE_SRGB,pixels,ANDROID_BITMAP_COMPRESS_FORMAT_PNG,101,NULL,NULL,123};
  pending_dispatch(env,expected,klj_bionic_graphics_bindings(),16,klj_bionic_graphics_dispatch,&arguments,sizeof arguments);
  assert(arguments.result==ANDROID_BITMAP_RESULT_BAD_PARAMETER);
 }
 if(groups&PROBE_EGL) {
  struct {int result;} arguments={123};
  pending_dispatch(env,expected,klj_bionic_egl_bindings(),66,klj_bionic_egl_dispatch,&arguments,sizeof arguments);
  assert(arguments.result==EGL_SUCCESS);
 }
 (*env)->ExceptionClear(env);(*env)->DeleteLocalRef(env,expected);(*env)->DeleteLocalRef(env,type);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeThreadProbe_call(
 JNIEnv *env,jclass type,jstring core,jstring file,jobject preparation) {
 jstring name=(*env)->NewStringUTF(env,"libnative-thread-client.so");
 image=klj_load_prepared(env,type,core,file,name,preparation);
 (*env)->DeleteLocalRef(env,name);if(!image||(*env)->ExceptionCheck(env))return;
 JNIEnv *outer=NULL;JavaVM *vm=NULL;
 assert(klj_native_image_jni_context(env,image,&outer,&vm)==1&&outer&&vm);
 void *entry=klj_guest_image_symbol(image,"native_thread_exercise");
 void *forbidden=klj_guest_image_symbol(image,"native_thread_forbidden");assert(entry&&forbidden);
 pending_controls(env,(uintptr_t)forbidden);
 NativeThreadCall call={.vm=vm,.groups=groups};uint64_t result=0;
 int64_t status=klj_bionic_runtime_invoke(runtime,env,(uintptr_t)entry,(uintptr_t)&call,&result);
 fprintf(stderr,"Native pthread result status=%lld line=%llu workerLine=%d callbackLine=%d guestTid=%d physical=%llu callbacks=%d detached=%d/%d/%d pins=%u/%u hostCalls=%u\n",
  (long long)status,(unsigned long long)result,call.line,call.callback_line,call.worker_tid,
  (unsigned long long)native_physical,call.callbacks,call.before,call.inside,call.after,borrowed,released,native_calls);
 assert(!status&&!result&&!(*env)->ExceptionCheck(env));
 assert(callbacks>=3&&borrowed==3&&released<=3&&native_calls>0);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeThreadProbe_complete(JNIEnv *env,jclass ignored) {
 (void)ignored;
 /* Original Bionic join observes clear_child_tid. Darwin then runs its own
  * pthread-key destructors, so observe that separate physical retirement. */
 for(unsigned i=0;i<2000&&released<borrowed;i++)usleep(1000);
 assert(borrowed==3&&released==3);
 int64_t status=-16;
 for(unsigned i=0;i<2000&&status==-16;i++){status=klj_bionic_runtime_try_close(runtime,env);if(status==-16)usleep(1000);}
 assert(!status);fprintf(stderr,"Native pthread descriptor leases retired=%u/%u\n",released,borrowed);
}
