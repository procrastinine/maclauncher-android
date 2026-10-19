#include <jni.h>
#import <Foundation/Foundation.h>
#include "audio-device.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <climits>
#include <algorithm>
static JavaVM *vm;static jclass eventHandler;static jmethodID postEvent;
static std::unordered_map<jlong,jobject> callbacks;static jlong nextCallback=1;
static KljAudioDeviceMonitor *monitor;
static std::atomic<uint64_t> epoch{0};static uint64_t nextEpoch=1;static std::mutex control,lifecycle;static bool installed;
static void fail(JNIEnv*env,const char*message){if(!env->ExceptionCheck())env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),message);}
// There is no Android audio-policy service. Preserve its ordinary NO_INIT
// result so original AudioAttributes can use AOSP's legacy stream fallback.
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_native_1list_1audio_1product_1strategies(JNIEnv*,jclass,jobject){return -19;}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_native_1list_1audio_1volume_1groups(JNIEnv*,jclass,jobject){return -19;}
// Android 16 system/media audio.h format limits (f01e84b958fb6a887dc0e74e4b5ebd159f03860a).
// These bound framework format values; they do not advertise a device format.
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_native_1getMaxChannelCount(JNIEnv*,jclass){return 12;}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_native_1getMaxSampleRate(JNIEnv*,jclass){return 192000;}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_native_1getMinSampleRate(JNIEnv*,jclass){return 4000;}

struct DeviceDescription {KljAudioDevice value{};~DeviceDescription(){klj_audio_device_release(&value);}};
static bool defaultDevice(KljAudioDevice *description){uint32_t id=0;return !klj_audio_device_default_output(&id)&&id&&!klj_audio_device_get(id,0,description);}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_getDeviceConnectionState(JNIEnv*env,jclass,jint requested,jstring location){
 uint32_t *devices=nullptr;size_t count=0;if(klj_audio_device_list(&devices,&count))return 0;
 jclass descriptor=env->FindClass("android/media/AudioDeviceInfo");if(!descriptor){free(devices);return 0;}
 jmethodID internal=env->GetStaticMethodID(descriptor,"convertDeviceTypeToInternalDevice","(I)I");if(!internal){env->DeleteLocalRef(descriptor);free(devices);return 0;}
 CFStringRef wanted=nullptr;if(location&&env->GetStringLength(location)>0){const jchar*chars=env->GetStringChars(location,nullptr);if(!chars){env->DeleteLocalRef(descriptor);free(devices);return 0;}wanted=CFStringCreateWithCharacters(kCFAllocatorDefault,chars,env->GetStringLength(location));env->ReleaseStringChars(location,chars);}
 jint connected=0;
 for(size_t i=0;i<count;i++){
  DeviceDescription device;if(klj_audio_device_get(devices[i],0,&device.value)||!device.value.alive||!device.value.channels)continue;
  jint type=env->CallStaticIntMethod(descriptor,internal,(jint)device.value.type);if(env->ExceptionCheck())break;if(type!=requested)continue;
  if(wanted&&(!device.value.uid||!CFEqual(device.value.uid,wanted)))continue;
  connected=1;break;
 }
 free(devices);if(wanted)CFRelease(wanted);env->DeleteLocalRef(descriptor);return connected;
}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_getPrimaryOutputSamplingRate(JNIEnv*,jclass){
 DeviceDescription device;return defaultDevice(&device.value)&&device.value.rate>0&&device.value.rate<=INT_MAX?jint(std::lround(device.value.rate)):0;
}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_getPrimaryOutputFrameCount(JNIEnv*,jclass){
 DeviceDescription device;return defaultDevice(&device.value)&&device.value.bufferFrames<=INT_MAX?jint(device.value.bufferFrames):0;
}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_getOutputLatency(JNIEnv*,jclass,jint){
 DeviceDescription device;if(!defaultDevice(&device.value)||device.value.rate<=0)return 0;
 const double milliseconds=(double(device.value.latencyFrames)+device.value.safetyFrames+device.value.bufferFrames)*1000/device.value.rate;
 return jint(std::min(double(INT_MAX),std::ceil(milliseconds)));
}
static jstring string(JNIEnv*env,CFStringRef value){if(!value)return env->NewStringUTF("");CFIndex length=CFStringGetLength(value);if(length>4096){fail(env,"Audio device text exceeds supported bounds");return nullptr;}std::vector<UniChar> chars(length);CFStringGetCharacters(value,CFRangeMake(0,length),chars.data());return env->NewString((jchar*)chars.data(),(jsize)length);}
static jobject snapshot(JNIEnv*env){@autoreleasepool{
 uint32_t id=0;if(klj_audio_device_default_output(&id)){fail(env,"CoreAudio default output query failed");return nullptr;}
 if(!id)return nullptr;
 DeviceDescription device;if(klj_audio_device_get(id,0,&device.value)||!device.value.name||!device.value.uid||device.value.rate<1||device.value.rate>768000||device.value.channels>INT_MAX){fail(env,"CoreAudio output description is unavailable");return nullptr;}
 jstring name=string(env,device.value.name);if(env->ExceptionCheck())return nullptr;jstring uid=string(env,device.value.uid);if(env->ExceptionCheck())return nullptr;
 jclass factory=env->FindClass("maclauncher/android/media/HostAudioDevices");if(!factory)return nullptr;
 jmethodID create=env->GetStaticMethodID(factory,"outputPort","(IILjava/lang/String;Ljava/lang/String;II)Landroid/media/AudioDevicePort;");if(!create)return nullptr;
 return env->CallStaticObjectMethod(factory,create,(jint)id,(jint)device.value.type,name,uid,(jint)std::lround(device.value.rate),(jint)device.value.channels);
}}
static bool resultGeneration(JNIEnv*env,jintArray target,jint value){
 if(!target||env->GetArrayLength(target)<1){fail(env,"Audio generation buffer is missing");return false;}
 env->SetIntArrayRegion(target,0,1,&value);return !env->ExceptionCheck();
}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_listAudioPorts(JNIEnv*env,jclass,jobject ports,jintArray revision){
 if(!ports){fail(env,"Audio port list is missing");return -22;}
 jint current=jint(klj_audio_device_generation());jobject port=snapshot(env);if(env->ExceptionCheck())return -1;
 if(port){jclass list=env->GetObjectClass(ports);if(!list)return -1;jmethodID add=env->GetMethodID(list,"add","(Ljava/lang/Object;)Z");if(!add)return -1;env->CallBooleanMethod(ports,add,port);if(env->ExceptionCheck())return -1;env->DeleteLocalRef(port);env->DeleteLocalRef(list);}
 return resultGeneration(env,revision,current)?0:-22;
}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_media_HostAudioSystem_listAudioPatches(JNIEnv*env,jclass,jobject patches,jintArray revision){
 if(!patches){fail(env,"Audio patch list is missing");return -22;}
 // Output players use the host mixer; there are no Android-owned port patches.
 return resultGeneration(env,revision,jint(klj_audio_device_generation()))?0:-22;
}
static void deviceChanged(void *owner){
 uint64_t token=(uint64_t)(uintptr_t)owner;
 if(epoch.load()!=token)return;JNIEnv*current=nullptr;bool attached=false;
 jint status=vm->GetEnv((void**)&current,JNI_VERSION_1_8);
 if(status==JNI_EDETACHED){if(vm->AttachCurrentThreadAsDaemon((void**)&current,nullptr)!=JNI_OK)return;attached=true;}else if(status!=JNI_OK)return;
 std::vector<jobject> targets;
 {std::lock_guard guard(control);if(epoch.load()==token)for(auto &[id,reference]:callbacks){jobject local=current->NewLocalRef(reference);if(!local)break;targets.push_back(local);}}
 if(current->ExceptionCheck()){current->ExceptionDescribe();current->ExceptionClear();}
 for(jobject target:targets){if(epoch.load()==token){current->CallStaticVoidMethod(eventHandler,postEvent,target,1,0,0,nullptr);if(current->ExceptionCheck()){current->ExceptionDescribe();current->ExceptionClear();}}current->DeleteLocalRef(target);}
 if(attached)vm->DetachCurrentThread();
}
static bool startMonitor(JNIEnv*env,jclass cls){
 if(installed)return true;
 if(!vm){
  JavaVM *candidateVM=nullptr;if(env->GetJavaVM(&candidateVM)!=JNI_OK){fail(env,"JVM is unavailable");return false;}
  jmethodID candidatePost=env->GetStaticMethodID(cls,"postEventFromNative","(Ljava/lang/Object;IIILjava/lang/Object;)V");if(!candidatePost)return false;
  jclass candidateHandler=(jclass)env->NewGlobalRef(cls);if(!candidateHandler)return false;
  vm=candidateVM;eventHandler=candidateHandler;postEvent=candidatePost;
 }
 uint64_t token=nextEpoch++;epoch.store(token);
 monitor=klj_audio_device_monitor(deviceChanged,(void*)(uintptr_t)token);
 if(!monitor){epoch.store(0);fail(env,"CoreAudio output monitoring could not start");return false;}
 installed=true;return true;
}
static void stopMonitor(JNIEnv*env){
 epoch.store(0);if(!installed)return;
 if(klj_audio_device_monitor_close(monitor)){fail(env,"CoreAudio output monitor could not stop");return;}
 installed=false;monitor=nullptr;
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_HostAudioPortEventHandler_native_1setup(JNIEnv*env,jclass,jobject owner,jobject reference){
 if(!reference){fail(env,"Audio callback reference is missing");return;}
 jclass cls=env->GetObjectClass(owner);if(!cls)return;jfieldID field=env->GetFieldID(cls,"mJniCallback","J");if(!field)return;
 std::lock_guard ownerGuard(lifecycle);if(env->GetLongField(owner,field)!=0)return;
 {std::lock_guard guard(control);if(nextCallback==INT64_MAX){fail(env,"Audio callback identity space exhausted");return;}}
 jobject retained=env->NewGlobalRef(reference);if(!retained)return;
 if(!startMonitor(env,cls)){env->DeleteGlobalRef(retained);return;}
 jlong handle=nextCallback++;{std::lock_guard guard(control);callbacks.emplace(handle,retained);}env->SetLongField(owner,field,handle);
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_media_HostAudioPortEventHandler_native_1finalize(JNIEnv*env,jclass,jobject owner){
 jclass cls=env->GetObjectClass(owner);if(!cls)return;jfieldID field=env->GetFieldID(cls,"mJniCallback","J");if(!field)return;
 std::lock_guard ownerGuard(lifecycle);jlong handle=env->GetLongField(owner,field);env->SetLongField(owner,field,0);bool empty;
 {std::lock_guard guard(control);auto found=callbacks.find(handle);if(found!=callbacks.end()){env->DeleteGlobalRef(found->second);callbacks.erase(found);}empty=callbacks.empty();}
 // CoreAudio may drain queued delivery while unregistering. Never hold the
 // callback map lock across an operation that can wait for that delivery.
 if(empty)stopMonitor(env);
}
