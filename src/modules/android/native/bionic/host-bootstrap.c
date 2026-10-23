#include "runtime-host.h"
#include "image-loader.h"
#include "native-image-provider.h"
#include "jni-provider.h"
#include "ndk-host.h"
#include "graphics-host.h"
#include "looper-host.h"
#include "egl-host.h"
#include "aaudio-host.h"
#include "choreographer-host.h"
#include "native-import-registry.h"
#include "read-fd.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static KljBionicRuntime *runtime;
static const KljBionicReadFdApi *fd_owner;
static const KljBionicPollFdApi *poll_owner;
extern void *kl_ndk_lookup(const char *name);
extern void *kl_aaudio_lookup(const char *name);
static JNIEnv *environment(void *unused,JNIEnv *real,JNIEnv *outer){
 (void)unused;return runtime&&klj_bionic_jni_environment(klj_bionic_runtime_jni_owner(runtime),real)==outer?real:NULL;
}
static int64_t invoke(void *unused,JNIEnv *env,uintptr_t caller,KljCallPacket *packet){
 (void)unused;const void *identity=klj_jni_provider_identity((void*)caller);if(!identity)return -22;
 return klj_native_image_invoke(env,(jlong)(uintptr_t)identity,packet)?packet->status:-22;
}
static int64_t read_lease(void *unused,int fd,int *native,void **lease){(void)unused;return fd_owner->borrow(fd,native,lease);}
static void release_lease(void *unused,void *lease){(void)unused;int saved=errno;int64_t status=fd_owner->release(lease);if(status)fprintf(stderr,"Android native descriptor release failed (%lld)\n",(long long)status);errno=saved;}
static int64_t host_call(void *unused,KljBionicRuntime *owner,JNIEnv *env,uint32_t code,uint64_t raw,uint64_t length){
 (void)unused;if(owner!=runtime)return -22;
 if(code==KLJ_BIONIC_NDK_HOSTCALL)return length==sizeof(KljBionicNdkPacket)?klj_bionic_ndk_dispatch(env,(void*)(uintptr_t)raw):-22;
 if(code==KLJ_BIONIC_GRAPHICS_HOSTCALL)return length==sizeof(KljBionicNdkPacket)?klj_bionic_graphics_dispatch(env,(void*)(uintptr_t)raw):-22;
 if(code==KLJ_BIONIC_LOOPER_HOSTCALL)return length==sizeof(KljBionicNdkPacket)?klj_bionic_looper_dispatch(env,(void*)(uintptr_t)raw):-22;
 if(code==KLJ_BIONIC_EGL_HOSTCALL)return length==sizeof(KljBionicNdkPacket)?klj_bionic_egl_dispatch(env,(void*)(uintptr_t)raw):-22;
 if(code==KLJ_BIONIC_AAUDIO_HOSTCALL)return length==sizeof(KljBionicNdkPacket)?klj_bionic_aaudio_dispatch(env,(void*)(uintptr_t)raw):-22;
 if(code==KLJ_BIONIC_CHOREOGRAPHER_HOSTCALL)return length==sizeof(KljBionicNdkPacket)?klj_bionic_choreographer_dispatch(env,(void*)(uintptr_t)raw):-22;
 return -38;
}
static int64_t install(JNIEnv *env,const char *name,const void *bindings){
 KljBionicSymbol symbol={sizeof(symbol),1,klj_bionic_runtime_guest(runtime)->helper.handle,name,NULL,NULL,0};
 int64_t status=klj_bionic_runtime_symbol(runtime,env,&symbol);uint64_t result=0;
 if(!status)status=klj_bionic_runtime_invoke(runtime,env,(uintptr_t)symbol.symbol,(uintptr_t)bindings,&result);
 return status?status:(int64_t)result;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_BionicNativeRuntime_prepare0(JNIEnv *env,jclass ignored,jobjectArray paths){
 if(runtime||!paths||(*env)->GetArrayLength(env,paths)!=9)return -22;
 const char *text[9]={0};jstring values[9]={0};int64_t status=0;
 for(unsigned i=0;i<9;i++){
  values[i]=(*env)->GetObjectArrayElement(env,paths,i);if((*env)->ExceptionCheck(env)||!values[i]){status=-22;break;}
  text[i]=(*env)->GetStringUTFChars(env,values[i],NULL);if(!text[i]){status=-12;break;}
 }
 if(!status&&strcmp(text[7],"0")&&strcmp(text[7],"1"))status=-22;
 if(!status){
  extern char **environ;size_t count=0;while(environ[count])count++;
  const char **guest_environment=calloc(count+1,sizeof(*guest_environment));
  if(!guest_environment)status=-12;
  else{
   size_t length=0;for(size_t i=0;i<count;i++){
    if(!strncmp(environ[i],"LD_PRELOAD=",11)||!strncmp(environ[i],"LD_LIBRARY_PATH=",16)||!strncmp(environ[i],"DYLD_",5))continue;
    guest_environment[length++]=environ[i];
   }
   KljBionicRuntimeOptions options={sizeof(options),1,text[0],text[1],text[2],text[3],text[4],text[5],text[6],guest_environment,length};
   status=klj_bionic_runtime_prepare(env,&options,&runtime);free(guest_environment);
  }
 }
 if(!status){
  void *abi=dlopen(text[0],RTLD_NOW|RTLD_NOLOAD);const KljBionicReadFdApi *(*getter)(void)=abi?dlsym(abi,"klj_bionic_read_fd_api_v1"):NULL;
  fd_owner=getter?getter():NULL;if(!fd_owner||fd_owner->size!=sizeof(*fd_owner)||fd_owner->version!=1||!fd_owner->borrow||!fd_owner->release)status=-38;
  const KljBionicPollFdApi *(*poll_getter)(void)=abi?dlsym(abi,"klj_bionic_poll_fd_api_v1"):NULL;
  poll_owner=poll_getter?poll_getter():NULL;if(!poll_owner||poll_owner->size!=sizeof(*poll_owner)||poll_owner->version!=1||!poll_owner->borrow||!poll_owner->release)status=-38;
 }
 if(!status)status=install(env,"klj_bionic_page_compat",(const void*)(uintptr_t)(text[7][0]=='1'));
 if(!status)status=install(env,"klj_bionic_application_libraries",text[8]);
 if(!status)status=klj_bionic_images_install(runtime,env);
 if(!status){extern void Java_maclauncher_android_bridge_NativeBinding_prepareNativeActivity(JNIEnv*,jclass);Java_maclauncher_android_bridge_NativeBinding_prepareNativeActivity(env,ignored);if((*env)->ExceptionCheck(env))status=-22;}
 if(!status){KljBionicNdkOwner owner={.resolve=klj_resolve_import,.environment=environment,.read_lease=read_lease,.release_lease=release_lease};status=klj_bionic_ndk_prepare(&owner);}
 if(!status){KljBionicGraphicsOwner owner={klj_resolve_import,environment,invoke,NULL};status=klj_bionic_graphics_prepare(&owner);}
 if(!status){
  KljBionicAAudioOwner owner={.resolve=kl_aaudio_lookup,.invoke=invoke,.context=NULL};
  if((*env)->GetJavaVM(env,&owner.vm)!=JNI_OK)status=-22;
  else status=klj_bionic_aaudio_prepare(&owner);
 }
 if(!status){KljBionicLooperOwner owner={kl_ndk_lookup,invoke,NULL,poll_owner};status=klj_bionic_looper_prepare(env,&owner);}
 if(!status){
  KljBionicChoreographerOwner owner={.resolve=kl_ndk_lookup,.invoke=invoke,.context=NULL};
  if((*env)->GetJavaVM(env,&owner.vm)!=JNI_OK)status=-22;
  else status=klj_bionic_choreographer_prepare(&owner);
 }
 if(!status){
  const char *slash=strrchr(text[5],'/');const char suffix[]="/libklepton-graphics.dylib";
  if(!slash)status=-22;
  else{
   size_t prefix=(size_t)(slash-text[5]);char *file=malloc(prefix+sizeof(suffix));
   if(!file)status=-12;
   else{
    memcpy(file,text[5],prefix);memcpy(file+prefix,suffix,sizeof(suffix));
    void *library=dlopen(file,RTLD_NOW|RTLD_NOLOAD);free(file);
    void *lookup=library?dlsym(library,"klj_native_egl_lookup"):NULL,*error=library?dlsym(library,"klj_native_egl_error"):NULL;
    if(!lookup||!error)status=-38;
    else{
     KljBionicEglOwner owner={.invoke=invoke,.context=NULL};
     memcpy(&owner.resolve,&lookup,sizeof(owner.resolve));memcpy(&owner.error,&error,sizeof(owner.error));
     KljAngleDebugOwnerInstall (*debug_owner)(void)=NULL;
     void *debug=dlsym(library,"klj_native_egl_debug_owner");
     if(debug)memcpy(&debug_owner,&debug,sizeof(debug_owner));
     owner.debug_callback=debug_owner?debug_owner():NULL;
     if((*env)->GetJavaVM(env,&owner.vm)!=JNI_OK)status=-22;
     else status=klj_bionic_egl_prepare(&owner);
    }
   }
  }
 }
 if(!status)status=klj_bionic_runtime_host_callback(runtime,host_call,NULL);
 if(!status)status=install(env,"klj_bionic_ndk_install",klj_bionic_ndk_bindings());
 if(!status)status=install(env,"klj_bionic_graphics_install",klj_bionic_graphics_bindings());
 if(!status)status=install(env,"klj_bionic_looper_install",klj_bionic_looper_bindings());
 if(!status)status=install(env,"klj_bionic_egl_install",klj_bionic_egl_bindings());
 if(!status)status=install(env,"klj_bionic_aaudio_install",klj_bionic_aaudio_bindings());
 if(!status)status=install(env,"klj_bionic_choreographer_install",klj_bionic_choreographer_bindings());
 for(unsigned i=0;i<9;i++){if(text[i])(*env)->ReleaseStringUTFChars(env,values[i],text[i]);if(values[i])(*env)->DeleteLocalRef(env,values[i]);}
 return status;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_BionicNativeRuntime_close0(JNIEnv *env,jclass ignored){(void)ignored;klj_bionic_looper_retire_current();return runtime?klj_bionic_runtime_try_close(runtime,env):0;}
