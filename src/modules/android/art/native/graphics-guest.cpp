#include <jni.h>
#include <android/binder_ibinder_jni.h>
#include <android/binder_libbinder.h>
#include <binder/IServiceManager.h>
#include <android/os/IServiceManager.h>
#include <binder/IBinder.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <cstdio>
static void error(JNIEnv*env,const char*message){jclass type=env->FindClass("java/io/IOException");if(type)env->ThrowNew(type,message);}
#include <gui/SurfaceControl.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <media/AudioSystem.h>
namespace android {Parcel* parcelForJavaObject(JNIEnv*,jobject);}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtPlatform_orientation(JNIEnv*env,jclass,jboolean landscape){
 auto authority=android::defaultServiceManager()->checkService(android::String16("maclauncher.art.window"));if(!authority){error(env,"Original native output authority is unavailable");return;}
 android::Parcel request,reply;request.writeInterfaceToken(android::String16("maclauncher.android.WindowOutput"));request.writeInt32(landscape?1:0);
 auto status=authority->transact(android::IBinder::FIRST_CALL_TRANSACTION+7,request,&reply);
 if(status){char message[128];snprintf(message,sizeof(message),"Native display orientation request failed: %d",status);error(env,message);}
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtPlatform_activity(JNIEnv*env,jclass,jobject manager){
 AIBinder*wrapped=AIBinder_fromJavaBinder(env,manager);if(!wrapped){error(env,"Activity owner is not a Binder");return;}
 auto endpoint=AIBinder_toPlatformBinder(wrapped);AIBinder_decStrong(wrapped);
 auto authority=android::defaultServiceManager()->checkService(android::String16("maclauncher.art.window"));
 if(!authority){error(env,"Original native output authority is unavailable");return;}
 android::ProcessState::self()->startThreadPool();
 android::Parcel request,reply;request.writeInterfaceToken(android::String16("maclauncher.android.WindowOutput"));request.writeStrongBinder(endpoint);
 auto status=authority->transact(android::IBinder::FIRST_CALL_TRANSACTION+1,request,&reply);
 if(status){error(env,"Original authority rejected the ActivityManager endpoint");return;}
 fprintf(stderr,"Canonical ActivityManager original Binder published for native UID observers\n");
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtPlatform_audioService(JNIEnv*env,jclass,jobject manager){
 AIBinder*wrapped=AIBinder_fromJavaBinder(env,manager);if(!wrapped){error(env,"Audio owner is not a Binder");return;}
 auto endpoint=AIBinder_toPlatformBinder(wrapped);AIBinder_decStrong(wrapped);
 auto authority=android::defaultServiceManager()->checkService(android::String16("maclauncher.art.window"));
 if(!authority){error(env,"Original native output authority is unavailable");return;}
 android::Parcel request,reply;request.writeInterfaceToken(android::String16("maclauncher.android.WindowOutput"));request.writeStrongBinder(endpoint);
 auto status=authority->transact(android::IBinder::FIRST_CALL_TRANSACTION+4,request,&reply);
 if(status){error(env,"Original authority rejected the audio service endpoint");return;}
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtPlatform_service(JNIEnv*env,jclass,jstring name,jobject manager){
 if(!name){error(env,"Service name is missing");return;}AIBinder*wrapped=AIBinder_fromJavaBinder(env,manager);if(!wrapped){error(env,"Service owner is not a Binder");return;}
 auto endpoint=AIBinder_toPlatformBinder(wrapped);AIBinder_decStrong(wrapped);
 auto authority=android::defaultServiceManager()->checkService(android::String16("maclauncher.art.window"));if(!authority){error(env,"Original native output authority is unavailable");return;}
 const jchar* chars=env->GetStringChars(name,nullptr);if(!chars)return;android::String16 service(reinterpret_cast<const char16_t*>(chars),env->GetStringLength(name));env->ReleaseStringChars(name,chars);
 android::Parcel request,reply;request.writeInterfaceToken(android::String16("maclauncher.android.WindowOutput"));request.writeString16(service);request.writeStrongBinder(endpoint);
 if(authority->transact(android::IBinder::FIRST_CALL_TRANSACTION+5,request,&reply))error(env,"Original authority rejected the service endpoint");
}
extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_ArtPlatform_connectivity(JNIEnv*env,jclass){
 auto manager=android::defaultServiceManager();auto endpoint=manager->checkService(android::String16("connectivity"));
 if(!endpoint){
  // The C++ convenience lookup folds an absent service and a failed original
  // directory transaction into nullptr. Preserve that distinction on failure.
  auto directory=android::interface_cast<android::os::IServiceManager>(android::IInterface::asBinder(manager));
  if(directory){
   android::os::Service value;auto status=directory->checkService2("connectivity",&value);
   const char*entry="unavailable";
   if(status.isOk())entry=value.getTag()==android::os::Service::Tag::serviceWithMetadata?(value.get<android::os::Service::Tag::serviceWithMetadata>().service?"binder":"absent"):"accessor";
   const auto window=manager->checkService(android::String16("maclauncher.art.window"));
   fprintf(stderr,"Original connectivity lookup failed: directory=%.512s entry=%s output=%s\n",status.toString8().c_str(),entry,window?"available":"absent");
  }else fprintf(stderr,"Original connectivity lookup failed: native directory is unavailable\n");
  error(env,"Application connectivity owner is unavailable");return nullptr;
 }
 AIBinder*wrapped=AIBinder_fromPlatformBinder(endpoint);if(!wrapped){error(env,"Application connectivity Binder could not be retained");return nullptr;}
 jobject result=AIBinder_toJavaBinder(env,wrapped);AIBinder_decStrong(wrapped);return result;
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtPlatform_network(JNIEnv*env,jclass,jint netId,jint state){
 auto authority=android::defaultServiceManager()->checkService(android::String16("maclauncher.art.window"));if(!authority){error(env,"Native resolver authority is unavailable");return;}
 android::Parcel request,reply;request.writeInterfaceToken(android::String16("maclauncher.android.WindowOutput"));request.writeInt32(netId);request.writeInt32(state);
 auto result=authority->transact(android::IBinder::FIRST_CALL_TRANSACTION+6,request,&reply);if(result!=android::OK){char text[96];snprintf(text,sizeof(text),"Original DNS resolver configuration failed: %d",result);error(env,text);}
}
extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_ArtPlatform_output(JNIEnv*env,jclass){
 auto service=android::defaultServiceManager()->checkService(android::String16("maclauncher.art.window"));if(!service){error(env,"Original native output capability is unavailable");return nullptr;}
 android::Parcel request,reply;request.writeInterfaceToken(android::String16("maclauncher.android.WindowOutput"));auto status=service->transact(android::IBinder::FIRST_CALL_TRANSACTION,request,&reply);
 if(status){error(env,"Original window authority rejected the caller");return nullptr;}
 jclass parcelType=env->FindClass("android/os/Parcel");jobject parcel=env->CallStaticObjectMethod(parcelType,env->GetStaticMethodID(parcelType,"obtain","()Landroid/os/Parcel;"));if(env->ExceptionCheck())return nullptr;
 auto target=android::parcelForJavaObject(env,parcel);target->writeString8(android::String8("MacLauncher Android output"));target->writeInt32(0);target->writeInt32(0);target->writeInt32(1);status=target->appendFrom(&reply,0,reply.dataSize());target->setDataPosition(0);
 jclass type=env->FindClass("android/view/SurfaceControl");jobject result=nullptr;if(status==android::OK){result=env->NewObject(type,env->GetMethodID(type,"<init>","()V"));env->CallVoidMethod(result,env->GetMethodID(type,"readFromParcel","(Landroid/os/Parcel;)V"),parcel);}else error(env,"Original SurfaceControl parcel could not be copied");
 if(!env->ExceptionCheck())env->CallVoidMethod(parcel,env->GetMethodID(parcelType,"recycle","()V"));env->DeleteLocalRef(parcel);return result;
}

#include <unistd.h>
#include <cerrno>
struct TextControl {int32_t kind;float bounds[4];};
static void textControl(JNIEnv*env,const TextControl&control){
 const char*bytes=reinterpret_cast<const char*>(&control);size_t remaining=sizeof(control);
 while(remaining){ssize_t count=write(7,bytes,remaining);if(count<0&&errno==EINTR)continue;if(count<=0){error(env,"Native text input control capability failed");return;}bytes+=count;remaining-=size_t(count);}
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostTextInput_enableNative(JNIEnv*env,jclass,jboolean enabled){textControl(env,{enabled?1:0,{}});}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostTextInput_areaNative(JNIEnv*env,jclass,jfloat left,jfloat top,jfloat right,jfloat bottom){textControl(env,{2,{left,top,right,bottom}});}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_HostAudioManager_nativeStreamVolume(JNIEnv*env,jclass,jint stream,jint index){
 auto authority=android::defaultServiceManager()->checkService(android::String16("maclauncher.art.window"));if(!authority){error(env,"Native audio settings authority is unavailable");return;}
 android::Parcel request,reply;request.writeInterfaceToken(android::String16("maclauncher.android.WindowOutput"));request.writeInt32(stream);request.writeInt32(index);
 auto status=authority->transact(android::IBinder::FIRST_CALL_TRANSACTION+3,request,&reply);if(status!=android::OK){char message[128];snprintf(message,sizeof(message),"Original AudioSystem stream volume failed: %d",status);error(env,message);}
}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_HostAudioManager_nativeStreamActive(JNIEnv*,jclass,jint stream,jint past){
 bool active=false;
 android::AudioSystem::isStreamActive(static_cast<audio_stream_type_t>(stream),&active,static_cast<uint32_t>(past));
 return active?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtPlatform_audio(JNIEnv*env,jclass,jintArray ranges){
 if(!ranges){error(env,"Shared audio ranges are missing");return;}const auto count=env->GetArrayLength(ranges);if(count%3){error(env,"Shared audio ranges are malformed");return;}
 auto authority=android::defaultServiceManager()->checkService(android::String16("maclauncher.art.window"));if(!authority){error(env,"Native audio settings authority is unavailable");return;}
 android::Parcel request,reply;request.writeInterfaceToken(android::String16("maclauncher.android.WindowOutput"));request.writeInt32(count/3);
 auto values=env->GetIntArrayElements(ranges,nullptr);if(!values)return;for(jsize i=0;i<count;++i)request.writeInt32(values[i]);env->ReleaseIntArrayElements(ranges,values,JNI_ABORT);
 auto status=authority->transact(android::IBinder::FIRST_CALL_TRANSACTION+2,request,&reply);if(status!=android::OK){char message[128];snprintf(message,sizeof(message),"Original AudioSystem range initialization failed: %d",status);error(env,message);}
}
