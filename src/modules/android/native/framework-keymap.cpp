#include <jni.h>
#include <dlfcn.h>
#include <memory>
#include <string>
#include "NativeKeyCharacterMap.h"
extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostInput_loadKeyboardMap(JNIEnv*env,jclass,jstring library,jstring text){
 const char*file=env->GetStringUTFChars(library,nullptr);if(!file)return nullptr;void*image=dlopen(file,RTLD_NOW|RTLD_LOCAL);env->ReleaseStringUTFChars(library,file);
 using Map=android::KeyCharacterMap;using Load=android::base::Result<std::shared_ptr<Map>>(*)(const std::string&,const char*,Map::Format);
 auto load=image?(Load)dlsym(image,"_ZN7android15KeyCharacterMap12loadContentsERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEPKcNS0_6FormatE"):nullptr;
 if(!load){if(image)dlclose(image);auto type=env->FindClass("java/lang/UnsatisfiedLinkError");if(type)env->ThrowNew(type,"Android native keyboard parser is unavailable");return nullptr;}
 const char*contents=env->GetStringUTFChars(text,nullptr);if(!contents){dlclose(image);return nullptr;}
 auto result=load("Virtual.kcm",contents,Map::Format::BASE);env->ReleaseStringUTFChars(text,contents);
 if(!result.ok()){auto type=env->FindClass("java/lang/IllegalArgumentException");if(type)env->ThrowNew(type,result.error().message().c_str());dlclose(image);return nullptr;}
 auto owner=std::make_unique<android::NativeKeyCharacterMap>(-1,std::make_unique<Map>(**result));
 auto type=env->FindClass("android/view/KeyCharacterMap");auto ctor=type?env->GetMethodID(type,"<init>","(J)V"):nullptr;
 jobject object=ctor?env->NewObject(type,ctor,(jlong)(uintptr_t)owner.get()):nullptr;if(object)owner.release();if(type)env->DeleteLocalRef(type);dlclose(image);return object;
}
