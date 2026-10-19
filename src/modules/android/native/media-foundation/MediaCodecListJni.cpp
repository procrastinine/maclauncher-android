/*
 * Copyright 2012, The Android Open Source Project
 * Copyright 2026, MacLauncher contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
// frameworks/base/media/jni/android_media_MediaCodecList.cpp, Android 16 source
// 99b01a65cc4c104933788b3143285ab6bae65827. Its native object construction
// contract is adapted here to the same macOS device owner, without a second
// Android media-server or a second set of Java capability calculations.
// The original MediaCodecList JNI object construction contract, backed by the
// same native descriptor owner as MediaCodec. Java capability calculation,
// filtering, aliases and format matching remain in the original framework.
#include "MediaCodecCatalog.h"
#include <jni.h>
#include <media/NdkMediaFormat.h>
#include <nativehelper/scoped_local_ref.h>
#include <memory>
#include <new>
#include <cstring>
#include <cerrno>
#include <string>

extern "C" jobject klj_media_format_map(JNIEnv*,AMediaFormat*);
namespace {
using Format=std::unique_ptr<AMediaFormat,decltype(&AMediaFormat_delete)>;
void fail(JNIEnv *env,const char *type,const char *message){
    if(env->ExceptionCheck())return;
    ScopedLocalRef<jclass> cls(env,env->FindClass(type));if(cls.get())env->ThrowNew(cls.get(),message);
}
const KLJCodecDescriptor *descriptor(JNIEnv *env,jint index){
    const auto codecs=klj_codec_catalog();
    if(index<0||static_cast<size_t>(index)>=codecs.size()){
        fail(env,"java/lang/IllegalArgumentException","Invalid codec index");return nullptr;
    }
    return &codecs[index];
}
struct Utf {
    JNIEnv *env;jstring source;const char *value;
    Utf(JNIEnv *e,jstring s):env(e),source(s),value(s?e->GetStringUTFChars(s,nullptr):nullptr){if(!s)fail(e,"java/lang/IllegalArgumentException","Codec name or MIME type is null");}
    ~Utf(){if(value)env->ReleaseStringUTFChars(source,value);}
};
template<class F> auto checked(JNIEnv *env,F fn)->decltype(fn()){
    try{return fn();}catch(const std::bad_alloc&){fail(env,"java/lang/OutOfMemoryError","Codec discovery allocation failed");return {};}
}
jobject capabilities(JNIEnv *env,const KLJCodecDescriptor &codec){
    Format defaults(AMediaFormat_new(),AMediaFormat_delete),details(AMediaFormat_new(),AMediaFormat_delete);
    if(!defaults||!details){fail(env,"java/lang/OutOfMemoryError","Codec discovery allocation failed");return nullptr;}
    AMediaFormat_setString(defaults.get(),AMEDIAFORMAT_KEY_MIME,codec.mime);
    // Output dimensions are learned from each decoded CVPixelBuffer. Both byte
    // buffers and the shared Android Surface sink accept those format changes.
    AMediaFormat_setInt32(details.get(),"feature-adaptive-playback",1);
    if(codec.maxWidth&&codec.maxHeight){
        const auto size="2x2-"+std::to_string(codec.maxWidth)+"x"+std::to_string(codec.maxHeight);
        AMediaFormat_setString(details.get(),"size-range",size.c_str());
        AMediaFormat_setString(details.get(),"alignment","2x2");
        AMediaFormat_setString(details.get(),"block-size","16x16");
        const auto blocks="1-"+std::to_string(((codec.maxWidth+15)/16)*((codec.maxHeight+15)/16));
        AMediaFormat_setString(details.get(),"block-count-range",blocks.c_str());
    }
    ScopedLocalRef<jobject> defaultMap(env,klj_media_format_map(env,defaults.get()));if(env->ExceptionCheck())return nullptr;
    ScopedLocalRef<jobject> detailsMap(env,klj_media_format_map(env,details.get()));if(env->ExceptionCheck())return nullptr;
    ScopedLocalRef<jclass> profileClass(env,env->FindClass("android/media/MediaCodecInfo$CodecProfileLevel"));if(!profileClass.get())return nullptr;
    auto profile=env->GetFieldID(profileClass.get(),"profile","I");if(!profile)return nullptr;
    auto level=env->GetFieldID(profileClass.get(),"level","I");if(!level)return nullptr;
    ScopedLocalRef<jobjectArray> profiles(env,env->NewObjectArray(codec.profiles.size(),profileClass.get(),nullptr));if(!profiles.get())return nullptr;
    for(size_t i=0;i<codec.profiles.size();i++){
        ScopedLocalRef<jobject> entry(env,env->AllocObject(profileClass.get()));if(!entry.get())return nullptr;
        env->SetIntField(entry.get(),profile,codec.profiles[i].profile);
        env->SetIntField(entry.get(),level,codec.profiles[i].level);
        env->SetObjectArrayElement(profiles.get(),i,entry.get());if(env->ExceptionCheck())return nullptr;
    }
    ScopedLocalRef<jintArray> colors(env,env->NewIntArray(codec.colors.size()));if(!colors.get())return nullptr;
    env->SetIntArrayRegion(colors.get(),0,codec.colors.size(),codec.colors.data());if(env->ExceptionCheck())return nullptr;
    ScopedLocalRef<jclass> capsClass(env,env->FindClass("android/media/MediaCodecInfo$CodecCapabilities"));if(!capsClass.get())return nullptr;
    auto create=env->GetMethodID(capsClass.get(),"<init>","([Landroid/media/MediaCodecInfo$CodecProfileLevel;[IZLjava/util/Map;Ljava/util/Map;)V");if(!create)return nullptr;
    return env->NewObject(capsClass.get(),create,profiles.get(),colors.get(),JNI_FALSE,defaultMap.get(),detailsMap.get());
}
}
#define NATIVE(type,name) extern "C" JNIEXPORT type JNICALL Java_android_media_MediaCodecList_##name
NATIVE(void,native_1init)(JNIEnv *env,jclass){checked(env,[]{return klj_codec_catalog().size();});}
NATIVE(jint,native_1getCodecCount)(JNIEnv *env,jclass){return checked(env,[]{return static_cast<jint>(klj_codec_catalog().size());});}
NATIVE(jstring,getCodecName)(JNIEnv *env,jclass,jint index){return checked(env,[&]()->jstring{auto codec=descriptor(env,index);return codec?env->NewStringUTF(codec->name):nullptr;});}
NATIVE(jstring,getCanonicalName)(JNIEnv *env,jclass,jint index){return Java_android_media_MediaCodecList_getCodecName(env,nullptr,index);}
NATIVE(jint,getAttributes)(JNIEnv *env,jclass,jint index){return checked(env,[&]()->jint{auto codec=descriptor(env,index);return codec?(codec->hardware?8:4):0;});}
NATIVE(jobjectArray,getSupportedTypes)(JNIEnv *env,jclass,jint index){return checked(env,[&]()->jobjectArray{
    auto codec=descriptor(env,index);if(!codec)return nullptr;
    ScopedLocalRef<jclass> cls(env,env->FindClass("java/lang/String"));if(!cls.get())return nullptr;
    ScopedLocalRef<jstring> mime(env,env->NewStringUTF(codec->mime));if(!mime.get())return nullptr;
    return env->NewObjectArray(1,cls.get(),mime.get());
});}
NATIVE(jobject,getCodecCapabilities)(JNIEnv *env,jclass,jint index,jstring type){return checked(env,[&]()->jobject{
    auto codec=descriptor(env,index);if(!codec)return nullptr;Utf mime(env,type);if(!mime.value)return nullptr;
    if(strcasecmp(codec->mime,mime.value)){fail(env,"java/lang/IllegalArgumentException","Unsupported codec MIME type");return nullptr;}
    return capabilities(env,*codec);
});}
NATIVE(jobject,native_1getGlobalSettings)(JNIEnv *env,jclass){return checked(env,[&]()->jobject{
    Format format(AMediaFormat_new(),AMediaFormat_delete);return klj_media_format_map(env,format.get());
});}
NATIVE(jint,findCodecByName)(JNIEnv *env,jclass,jstring name){return checked(env,[&]()->jint{
    Utf text(env,name);if(!text.value)return -ENOENT;const auto codecs=klj_codec_catalog();
    for(size_t i=0;i<codecs.size();i++)if(!strcmp(codecs[i].name,text.value))return static_cast<jint>(i);
    return -ENOENT;
});}
