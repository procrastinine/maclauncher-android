/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2026 MacLauncher contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// Original MediaExtractor JNI surface and serialization, using the shared Mac
// extractor owner. No Java-side object registry or second demuxer is involved.
#include "MediaExtractorOwner.h"
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaFormatPriv.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AudioPresentationInfo.h>
#include <jni.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <vector>

extern "C" jobject klj_media_format_map(JNIEnv *,AMediaFormat *);

namespace {
std::mutex contextMutex;
jfieldID contextField;

void fail(JNIEnv *env,const char *type,const char *message=nullptr) {
    if(env->ExceptionCheck())return;
    jclass cls=env->FindClass(type);
    if(env->ExceptionCheck()||!cls)return;
    env->ThrowNew(cls,message);
    env->DeleteLocalRef(cls);
}
void allocationFailure(JNIEnv *env){fail(env,"java/lang/OutOfMemoryError","Media allocation failed");}
bool argumentStatus(JNIEnv *env,int64_t status) {
    if(status>=0)return true;
    fail(env,"java/lang/IllegalArgumentException","Extractor operation failed");return false;
}
struct Extractor {
    KLJExtractor *value=nullptr;
    Extractor(JNIEnv *env,jobject self) {
        {
            std::lock_guard<std::mutex> lock(contextMutex);
            if(contextField)value=reinterpret_cast<KLJExtractor *>(env->GetLongField(self,contextField));
            if(env->ExceptionCheck())value=nullptr;
            if(value)klj_extractor_retain(value);
        }
        if(!value)fail(env,"java/lang/IllegalStateException","Extractor is released");
    }
    ~Extractor(){if(value)klj_extractor_release(value);}
    explicit operator bool()const{return value!=nullptr;}
};
using Format=std::unique_ptr<AMediaFormat,decltype(&AMediaFormat_delete)>;
Format format(KLJExtractor *owner,int index){return Format(klj_extractor_format(owner,index),AMediaFormat_delete);}
struct Utf {
    JNIEnv *env;jstring string;const char *chars;
    Utf(JNIEnv *e,jstring s):env(e),string(s),chars(s?e->GetStringUTFChars(s,nullptr):nullptr){}
    ~Utf(){if(chars)env->ReleaseStringUTFChars(string,chars);}
};
template<typename T> struct Local {
    JNIEnv *env;T value;
    Local(JNIEnv *e,T v):env(e),value(v){}
    ~Local(){if(value)env->DeleteLocalRef(value);}
    operator T()const{return value;}
};

// Java callbacks can run on AVFoundation workers. Each temporary attachment is
// retired by its owner; Java-originating threads are never detached here.
struct AttachedEnv {
    JavaVM *vm;JNIEnv *env=nullptr;bool attached=false;
    explicit AttachedEnv(JavaVM *v):vm(v){
        jint result=vm->GetEnv(reinterpret_cast<void **>(&env),JNI_VERSION_1_6);
        if(result==JNI_EDETACHED){
            if(vm->AttachCurrentThread(&env,nullptr)==JNI_OK)attached=true;
            else env=nullptr;
        }else if(result!=JNI_OK)env=nullptr;
    }
    ~AttachedEnv(){if(attached)vm->DetachCurrentThread();}
};
struct JavaSource {
    JavaVM *vm=nullptr;jobject object=nullptr;
    jmethodID read=nullptr,size=nullptr,close=nullptr;
    std::atomic<bool> closed{false};
};
ssize_t javaRead(void *opaque,int64_t position,void *buffer,size_t capacity) {
    auto *source=static_cast<JavaSource *>(opaque);
    AttachedEnv attached(source->vm);JNIEnv *env=attached.env;
    if(!env||source->closed.load())return -2;
    if(!capacity)return 0;
    // One Java readAt call is bounded by its actual signed int API argument.
    jsize length=static_cast<jsize>(std::min(capacity,static_cast<size_t>(INT_MAX)));
    Local<jbyteArray> bytes(env,env->NewByteArray(length));
    if(env->ExceptionCheck()||!bytes.value){env->ExceptionClear();return -2;}
    jint count=env->CallIntMethod(source->object,source->read,static_cast<jlong>(position),bytes.value,0,length);
    if(env->ExceptionCheck()){env->ExceptionClear();return -2;}
    if(count>length||count<-1)return -2;
    if(count>0){
        env->GetByteArrayRegion(bytes.value,0,count,static_cast<jbyte *>(buffer));
        if(env->ExceptionCheck()){env->ExceptionClear();return -2;}
    }
    return count;
}
ssize_t javaSize(void *opaque) {
    auto *source=static_cast<JavaSource *>(opaque);
    AttachedEnv attached(source->vm);JNIEnv *env=attached.env;
    if(!env||source->closed.load())return -1;
    jlong size=env->CallLongMethod(source->object,source->size);
    if(env->ExceptionCheck()){env->ExceptionClear();return -1;}
    return size>=-1?static_cast<ssize_t>(size):-1;
}
void javaClose(void *opaque) {
    auto *source=static_cast<JavaSource *>(opaque);
    if(source->closed.exchange(true))return;
    AttachedEnv attached(source->vm);JNIEnv *env=attached.env;if(!env)return;
    // Cleanup may be reached while unwinding an unrelated JNI failure. Preserve
    // that exception, and never call a Java close with an exception pending.
    jthrowable pending=env->ExceptionOccurred();
    if(pending)env->ExceptionClear();
    env->CallVoidMethod(source->object,source->close);
    if(env->ExceptionCheck())env->ExceptionClear();
    if(pending){env->Throw(pending);env->DeleteLocalRef(pending);}
}
void javaDispose(void *opaque) {
    auto *source=static_cast<JavaSource *>(opaque);
    javaClose(source);
    AttachedEnv attached(source->vm);
    if(attached.env)attached.env->DeleteGlobalRef(source->object);
    delete source;
}
klj_media_source *javaSource(JNIEnv *env,jobject callback) {
    std::unique_ptr<JavaSource> data(new(std::nothrow) JavaSource);
    if(!data){allocationFailure(env);return nullptr;}
    if(env->GetJavaVM(&data->vm)!=JNI_OK){fail(env,"java/lang/IllegalStateException");return nullptr;}
    Local<jclass> cls(env,env->GetObjectClass(callback));
    if(env->ExceptionCheck()||!cls.value)return nullptr;
    data->read=env->GetMethodID(cls,"readAt","(J[BII)I");
    if(env->ExceptionCheck()||!data->read)return nullptr;
    data->size=env->GetMethodID(cls,"getSize","()J");
    if(env->ExceptionCheck()||!data->size)return nullptr;
    data->close=env->GetMethodID(cls,"close","()V");
    if(env->ExceptionCheck()||!data->close)return nullptr;
    data->object=env->NewGlobalRef(callback);
    if(env->ExceptionCheck()||!data->object)return nullptr;
    klj_media_source *source=klj_media_source_new();
    if(!source){env->DeleteGlobalRef(data->object);allocationFailure(env);return nullptr;}
    klj_media_source_userdata(source,data.release());
    klj_media_source_set_read(source,javaRead);
    klj_media_source_set_size(source,javaSize);
    klj_media_source_set_close(source,javaClose);
    klj_media_source_set_dispose(source,javaDispose);
    return source;
}
jobject javaFormat(JNIEnv *env,jobject self,int index) {
    Extractor owner(env,self);if(!owner)return nullptr;
    auto metadata=format(owner.value,index);
    if(!metadata){fail(env,"java/lang/IllegalArgumentException","Format is unavailable");return nullptr;}
    return klj_media_format_map(env,metadata.get());
}
void select(JNIEnv *env,jobject self,jint index,bool selected) {
    Extractor owner(env,self);if(!owner)return;
    if(index<0){fail(env,"java/lang/IllegalArgumentException");return;}
    argumentStatus(env,klj_extractor_select(owner.value,static_cast<size_t>(index),selected));
}
int64_t sampleResult(JNIEnv *env,int64_t result) {
    if(result<-1){argumentStatus(env,result);return -1;}return result;
}
jobject newObject(JNIEnv *env,const char *name) {
    Local<jclass> cls(env,env->FindClass(name));if(env->ExceptionCheck()||!cls.value)return nullptr;
    jmethodID init=env->GetMethodID(cls,"<init>","()V");if(env->ExceptionCheck()||!init)return nullptr;
    jobject result=env->NewObject(cls,init);if(env->ExceptionCheck())return nullptr;return result;
}
jobject boxed(JNIEnv *env,const char *name,const char *signature,jvalue value) {
    Local<jclass> cls(env,env->FindClass(name));if(env->ExceptionCheck()||!cls.value)return nullptr;
    jmethodID init=env->GetMethodID(cls,"<init>",signature);if(env->ExceptionCheck()||!init)return nullptr;
    jobject result=env->NewObjectA(cls,init,&value);if(env->ExceptionCheck())return nullptr;return result;
}
bool putMap(JNIEnv *env,jobject map,jmethodID put,const char *key,jobject value) {
    Local<jstring> name(env,env->NewStringUTF(key));if(env->ExceptionCheck()||!name.value)return false;
    Local<jobject> previous(env,env->CallObjectMethod(map,put,name.value,value));return !env->ExceptionCheck();
}
jobject byteBuffer(JNIEnv *env,const android::sp<android::ABuffer> &buffer) {
    if(buffer->size()>INT_MAX){allocationFailure(env);return nullptr;}
    jsize length=static_cast<jsize>(buffer->size());
    Local<jbyteArray> bytes(env,env->NewByteArray(length));if(env->ExceptionCheck()||!bytes.value)return nullptr;
    env->SetByteArrayRegion(bytes,0,length,reinterpret_cast<const jbyte *>(buffer->data()));if(env->ExceptionCheck())return nullptr;
    Local<jclass> cls(env,env->FindClass("java/nio/ByteBuffer"));if(env->ExceptionCheck()||!cls.value)return nullptr;
    jmethodID wrap=env->GetStaticMethodID(cls,"wrap","([B)Ljava/nio/ByteBuffer;");if(env->ExceptionCheck()||!wrap)return nullptr;
    jobject result=env->CallStaticObjectMethod(cls,wrap,bytes.value);if(env->ExceptionCheck())return nullptr;return result;
}
// Same AudioPresentation private constructor and label/ULocale serialization as
// original android_media_AudioPresentation.h, with exception checks per call.
jobject presentations(JNIEnv *env,AMediaFormat *metadata) {
    jobject list=newObject(env,"java/util/ArrayList");if(!list)return nullptr;
    void *data=nullptr;size_t length=0;
    if(!AMediaFormat_getBuffer(metadata,"audio-presentation-info",&data,&length))return list;
    if(!data||length<sizeof(uint32_t)){fail(env,"java/lang/IllegalArgumentException","Invalid audio presentation metadata");return nullptr;}
    android::AudioPresentationCollection values;
    std::istringstream input(std::string(static_cast<const char *>(data),length));
    input.exceptions(std::ios::badbit|std::ios::failbit);
    if(android::deserializeAudioPresentations(&input,&values)!=android::OK){
        fail(env,"java/lang/IllegalArgumentException","Invalid audio presentation metadata");return nullptr;
    }
    Local<jclass> listClass(env,env->GetObjectClass(list));if(env->ExceptionCheck())return nullptr;
    jmethodID add=env->GetMethodID(listClass,"add","(Ljava/lang/Object;)Z");if(env->ExceptionCheck()||!add)return nullptr;
    Local<jclass> localeClass(env,env->FindClass("android/icu/util/ULocale"));if(env->ExceptionCheck()||!localeClass.value)return nullptr;
    jmethodID localeInit=env->GetMethodID(localeClass,"<init>","(Ljava/lang/String;)V");if(env->ExceptionCheck()||!localeInit)return nullptr;
    Local<jclass> mapClass(env,env->FindClass("java/util/HashMap"));if(env->ExceptionCheck()||!mapClass.value)return nullptr;
    jmethodID mapInit=env->GetMethodID(mapClass,"<init>","()V");if(env->ExceptionCheck()||!mapInit)return nullptr;
    jmethodID put=env->GetMethodID(mapClass,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");if(env->ExceptionCheck()||!put)return nullptr;
    Local<jclass> presentationClass(env,env->FindClass("android/media/AudioPresentation"));if(env->ExceptionCheck()||!presentationClass.value)return nullptr;
    jmethodID init=env->GetMethodID(presentationClass,"<init>","(IILandroid/icu/util/ULocale;IZZZLjava/util/Map;)V");if(env->ExceptionCheck()||!init)return nullptr;
    for(const auto &value:values){
        Local<jobject> labels(env,env->NewObject(mapClass,mapInit));if(env->ExceptionCheck()||!labels.value)return nullptr;
        for(const auto &label:value.mLabels){
            Local<jstring> language(env,env->NewStringUTF(label.first.c_str()));if(env->ExceptionCheck()||!language.value)return nullptr;
            Local<jobject> locale(env,env->NewObject(localeClass,localeInit,language.value));if(env->ExceptionCheck()||!locale.value)return nullptr;
            Local<jstring> text(env,env->NewStringUTF(label.second.c_str()));if(env->ExceptionCheck()||!text.value)return nullptr;
            Local<jobject> previous(env,env->CallObjectMethod(labels.value,put,locale.value,text.value));if(env->ExceptionCheck())return nullptr;
        }
        Local<jstring> language(env,env->NewStringUTF(value.mLanguage.c_str()));if(env->ExceptionCheck()||!language.value)return nullptr;
        Local<jobject> locale(env,env->NewObject(localeClass,localeInit,language.value));if(env->ExceptionCheck()||!locale.value)return nullptr;
        Local<jobject> presentation(env,env->NewObject(presentationClass,init,
            static_cast<jint>(value.mPresentationId),static_cast<jint>(value.mProgramId),locale.value,
            static_cast<jint>(value.mMasteringIndication),static_cast<jboolean>(value.mAudioDescriptionAvailable),
            static_cast<jboolean>(value.mSpokenSubtitlesAvailable),static_cast<jboolean>(value.mDialogueEnhancementAvailable),labels.value));
        if(env->ExceptionCheck()||!presentation.value)return nullptr;
        env->CallBooleanMethod(list,add,presentation.value);if(env->ExceptionCheck())return nullptr;
    }
    return list;
}
} // namespace

extern "C" jobject klj_media_format_map(JNIEnv *env,AMediaFormat *metadata) {
    if(!metadata){fail(env,"java/lang/IllegalArgumentException");return nullptr;}
    try {
        android::sp<android::AMessage> message;android::AMediaFormat_getFormat(metadata,&message);
        if(!message){fail(env,"java/lang/IllegalArgumentException");return nullptr;}
        jobject map=newObject(env,"java/util/HashMap");if(!map)return nullptr;
        Local<jclass> cls(env,env->GetObjectClass(map));if(env->ExceptionCheck())return nullptr;
        jmethodID put=env->GetMethodID(cls,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");if(env->ExceptionCheck()||!put)return nullptr;
        // Preserve ConvertMessageToMap's public-key/type contract, including
        // rectangle expansion and owned byte-array copies for buffer metadata.
        for(size_t i=0;i<message->countEntries();++i){
            android::AMessage::Type type;const char *key=message->getEntryNameAt(i,&type);
            if(!std::strncmp(key,"android._",9))continue;
            Local<jobject> value(env,nullptr);jvalue scalar{};
            switch(type){
                case android::AMessage::kTypeInt32:{int32_t number;message->findInt32(key,&number);scalar.i=number;value.value=boxed(env,"java/lang/Integer","(I)V",scalar);break;}
                case android::AMessage::kTypeInt64:{int64_t number;message->findInt64(key,&number);scalar.j=number;value.value=boxed(env,"java/lang/Long","(J)V",scalar);break;}
                case android::AMessage::kTypeFloat:{float number;message->findFloat(key,&number);scalar.f=number;value.value=boxed(env,"java/lang/Float","(F)V",scalar);break;}
                case android::AMessage::kTypeString:{android::AString text;message->findString(key,&text);value.value=env->NewStringUTF(text.c_str());break;}
                case android::AMessage::kTypeBuffer:{android::sp<android::ABuffer> data;message->findBuffer(key,&data);value.value=byteBuffer(env,data);break;}
                case android::AMessage::kTypeRect:{
                    int32_t numbers[4];message->findRect(key,&numbers[0],&numbers[1],&numbers[2],&numbers[3]);
                    const char *suffixes[]={"-left","-top","-right","-bottom"};
                    for(int part=0;part<4;++part){
                        scalar.i=numbers[part];Local<jobject> number(env,boxed(env,"java/lang/Integer","(I)V",scalar));
                        if(env->ExceptionCheck()||!number.value)return nullptr;
                        if(!putMap(env,map,put,(std::string(key)+suffixes[part]).c_str(),number.value))return nullptr;
                    }
                    break;
                }
                default:break; // Original Java MediaFormat exposes no other AMessage types.
            }
            if(env->ExceptionCheck())return nullptr;
            if(value.value&&!putMap(env,map,put,key,value.value))return nullptr;
        }
        return map;
    }catch(const std::bad_alloc &){allocationFailure(env);return nullptr;}
}

#define NATIVE(type,name) extern "C" JNIEXPORT type JNICALL Java_android_media_MediaExtractor_##name
NATIVE(void,native_1init)(JNIEnv *env,jclass cls) {
    jfieldID field=env->GetFieldID(cls,"mNativeContext","J");if(env->ExceptionCheck()||!field)return;
    std::lock_guard<std::mutex> lock(contextMutex);contextField=field;
}
NATIVE(void,native_1setup)(JNIEnv *env,jobject self) {
    KLJExtractor *owner=klj_extractor_create();if(!owner){allocationFailure(env);return;}
    bool published=false;
    {
        std::lock_guard<std::mutex> lock(contextMutex);
        jlong old=contextField?env->GetLongField(self,contextField):0;
        if(!env->ExceptionCheck()&&contextField&&!old){
            env->SetLongField(self,contextField,reinterpret_cast<jlong>(owner));published=!env->ExceptionCheck();
        }
    }
    if(!published){klj_extractor_close(owner);klj_extractor_release(owner);fail(env,"java/lang/IllegalStateException");}
}
NATIVE(void,release)(JNIEnv *env,jobject self) {
    KLJExtractor *owner=nullptr;
    {
        std::lock_guard<std::mutex> lock(contextMutex);if(!contextField)return;
        owner=reinterpret_cast<KLJExtractor *>(env->GetLongField(self,contextField));if(env->ExceptionCheck())return;
        env->SetLongField(self,contextField,0);if(env->ExceptionCheck())return;
    }
    if(owner){klj_extractor_close(owner);klj_extractor_release(owner);}
}
NATIVE(void,native_1finalize)(JNIEnv *env,jobject self){Java_android_media_MediaExtractor_release(env,self);}
NATIVE(void,nativeSetDataSource)(JNIEnv *env,jobject self,jobject,jstring path,jobjectArray keys,jobjectArray values) {
    Extractor owner(env,self);if(!owner)return;
    if(!path||static_cast<bool>(keys)!=static_cast<bool>(values)){fail(env,"java/lang/IllegalArgumentException");return;}
    try {
        jsize count=keys?env->GetArrayLength(keys):0;if(env->ExceptionCheck())return;
        jsize valueCount=values?env->GetArrayLength(values):0;if(env->ExceptionCheck())return;
        if(count!=valueCount){fail(env,"java/lang/IllegalArgumentException");return;}
        std::vector<std::string> names,texts;names.reserve(count);texts.reserve(count);
        for(jsize i=0;i<count;++i){
            Local<jstring> key(env,static_cast<jstring>(env->GetObjectArrayElement(keys,i)));if(env->ExceptionCheck())return;
            Local<jstring> value(env,static_cast<jstring>(env->GetObjectArrayElement(values,i)));if(env->ExceptionCheck())return;
            if(!key.value||!value.value){fail(env,"java/lang/IllegalArgumentException");return;}
            Utf keyText(env,key);if(env->ExceptionCheck()||!keyText.chars)return;
            Utf valueText(env,value);if(env->ExceptionCheck()||!valueText.chars)return;
            names.emplace_back(keyText.chars);texts.emplace_back(valueText.chars);
        }
        std::vector<const char *> namePointers,valuePointers;
        for(jsize i=0;i<count;++i){namePointers.push_back(names[i].c_str());valuePointers.push_back(texts[i].c_str());}
        Utf pathText(env,path);if(env->ExceptionCheck()||!pathText.chars)return;
        int status=klj_extractor_set_path(owner.value,pathText.chars,namePointers.data(),valuePointers.data(),names.size());
        if(status<0)fail(env,"java/io/IOException","Failed to instantiate extractor");
    }catch(const std::bad_alloc &){allocationFailure(env);}
}
NATIVE(void,setDataSource__Ljava_io_FileDescriptor_2JJ)(JNIEnv *env,jobject self,jobject descriptor,jlong offset,jlong size) {
    Extractor owner(env,self);if(!owner)return;
    if(!descriptor){fail(env,"java/lang/IllegalArgumentException");return;}
    Local<jclass> cls(env,env->GetObjectClass(descriptor));if(env->ExceptionCheck()||!cls.value)return;
    jfieldID field=env->GetFieldID(cls,"fd","I");if(env->ExceptionCheck()||!field)return;
    jint fd=env->GetIntField(descriptor,field);if(env->ExceptionCheck())return;
    if(klj_extractor_set_fd(owner.value,fd,offset,size)<0)fail(env,"java/io/IOException","Failed to instantiate extractor");
}
NATIVE(void,setDataSource__Landroid_media_MediaDataSource_2)(JNIEnv *env,jobject self,jobject callback) {
    Extractor owner(env,self);if(!owner)return;
    if(!callback){fail(env,"java/lang/IllegalArgumentException");return;}
    klj_media_source *source=javaSource(env,callback);if(!source)return;
    int status=klj_extractor_set_source(owner.value,source);
    if(status<0)klj_media_source_close(source);
    klj_media_source_delete(source);
    if(status<0)fail(env,"java/io/IOException","Failed to instantiate extractor");
}
NATIVE(void,nativeSetMediaCas)(JNIEnv *env,jobject self,jobject binder) {
    Extractor owner(env,self);if(!owner)return;
    fail(env,binder?"java/lang/UnsupportedOperationException":"java/lang/IllegalArgumentException",
        binder?"Media CAS is unavailable":nullptr);
}
NATIVE(jint,getTrackCount)(JNIEnv *env,jobject self) {
    Extractor owner(env,self);if(!owner)return -1;
    size_t count=klj_extractor_count(owner.value);
    if(count>INT_MAX){fail(env,"java/lang/IllegalStateException","Track count exceeds Java range");return -1;}
    return static_cast<jint>(count);
}
NATIVE(jobject,getFileFormatNative)(JNIEnv *env,jobject self){return javaFormat(env,self,-1);}
NATIVE(jobject,getTrackFormatNative)(JNIEnv *env,jobject self,jint index){
    if(index<0){fail(env,"java/lang/IllegalArgumentException");return nullptr;}return javaFormat(env,self,index);
}
NATIVE(void,selectTrack)(JNIEnv *env,jobject self,jint index){select(env,self,index,true);}
NATIVE(void,unselectTrack)(JNIEnv *env,jobject self,jint index){select(env,self,index,false);}
NATIVE(void,seekTo)(JNIEnv *env,jobject self,jlong time,jint mode) {
    Extractor owner(env,self);if(!owner)return;
    if(mode<0||mode>2){fail(env,"java/lang/IllegalArgumentException");return;}
    argumentStatus(env,klj_extractor_seek(owner.value,time,mode));
}
NATIVE(jboolean,advance)(JNIEnv *env,jobject self) {
    Extractor owner(env,self);if(!owner)return JNI_FALSE;
    int result=klj_extractor_advance(owner.value);return argumentStatus(env,result)&&result>0?JNI_TRUE:JNI_FALSE;
}
NATIVE(jint,readSampleData)(JNIEnv *env,jobject self,jobject buffer,jint offset) {
    Extractor owner(env,self);if(!owner)return -1;
    if(!buffer||offset<0){fail(env,"java/lang/IllegalArgumentException");return -1;}
    Local<jclass> cls(env,env->GetObjectClass(buffer));if(env->ExceptionCheck()||!cls.value)return -1;
    jmethodID readOnly=env->GetMethodID(cls,"isReadOnly","()Z");if(env->ExceptionCheck()||!readOnly)return -1;
    jboolean readonly=env->CallBooleanMethod(buffer,readOnly);if(env->ExceptionCheck())return -1;
    if(readonly){fail(env,"java/lang/IllegalArgumentException","Buffer is read-only");return -1;}
    jlong capacity=env->GetDirectBufferCapacity(buffer);if(env->ExceptionCheck())return -1;
    jbyte *destination=nullptr;Local<jbyteArray> bytes(env,nullptr);jbyte *elements=nullptr;
    if(capacity>=0){
        destination=static_cast<jbyte *>(env->GetDirectBufferAddress(buffer));if(env->ExceptionCheck())return -1;
    }else{
        jmethodID array=env->GetMethodID(cls,"array","()[B");if(env->ExceptionCheck()||!array)return -1;
        jmethodID arrayOffset=env->GetMethodID(cls,"arrayOffset","()I");if(env->ExceptionCheck()||!arrayOffset)return -1;
        jmethodID capacityMethod=env->GetMethodID(cls,"capacity","()I");if(env->ExceptionCheck()||!capacityMethod)return -1;
        bytes.value=static_cast<jbyteArray>(env->CallObjectMethod(buffer,array));if(env->ExceptionCheck()||!bytes.value)return -1;
        jint base=env->CallIntMethod(buffer,arrayOffset);if(env->ExceptionCheck())return -1;
        capacity=env->CallIntMethod(buffer,capacityMethod);if(env->ExceptionCheck())return -1;
        jsize length=env->GetArrayLength(bytes);if(env->ExceptionCheck())return -1;
        if(base<0||capacity<0||base>length||capacity>length-base){fail(env,"java/lang/IllegalArgumentException");return -1;}
        if(offset>capacity){fail(env,"java/lang/IllegalArgumentException");return -1;}
        elements=env->GetByteArrayElements(bytes,nullptr);if(env->ExceptionCheck()||!elements)return -1;
        destination=elements+base;
    }
    ssize_t result=-ERANGE;
    if(offset<=capacity&&(destination||capacity==0)){
        result=klj_extractor_read(owner.value,destination?destination+offset:nullptr,static_cast<size_t>(capacity-offset));
    }
    if(elements)env->ReleaseByteArrayElements(bytes,elements,result>=0?0:JNI_ABORT);
    if(env->ExceptionCheck())return -1;
    if(result==-1)return -1;
    if(!argumentStatus(env,result))return -1;
    if(result>capacity-offset||result>INT_MAX-offset){fail(env,"java/lang/IllegalArgumentException","Invalid sample size");return -1;}
    jmethodID limit=env->GetMethodID(cls,"limit","(I)Ljava/nio/Buffer;");if(env->ExceptionCheck()||!limit)return -1;
    jmethodID position=env->GetMethodID(cls,"position","(I)Ljava/nio/Buffer;");if(env->ExceptionCheck()||!position)return -1;
    Local<jobject> limited(env,env->CallObjectMethod(buffer,limit,offset+static_cast<jint>(result)));if(env->ExceptionCheck())return -1;
    Local<jobject> positioned(env,env->CallObjectMethod(buffer,position,offset));if(env->ExceptionCheck())return -1;
    return static_cast<jint>(result);
}
NATIVE(jint,getSampleTrackIndex)(JNIEnv *env,jobject self){Extractor owner(env,self);return owner?static_cast<jint>(sampleResult(env,klj_extractor_sample_track(owner.value))):-1;}
NATIVE(jlong,getSampleTime)(JNIEnv *env,jobject self){
    Extractor owner(env,self);if(!owner)return -1;
    int64_t time=-1;int status=klj_extractor_sample_time_status(owner.value,&time);
    if(status<0)argumentStatus(env,status);return status==0?time:-1;
}
NATIVE(jlong,getSampleSize)(JNIEnv *env,jobject self){Extractor owner(env,self);return owner?sampleResult(env,klj_extractor_sample_size(owner.value)):-1;}
NATIVE(jint,getSampleFlags)(JNIEnv *env,jobject self){Extractor owner(env,self);return owner?static_cast<jint>(sampleResult(env,klj_extractor_sample_flags(owner.value))):-1;}
NATIVE(jboolean,getSampleCryptoInfo)(JNIEnv *env,jobject self,jobject) {
    Extractor owner(env,self);if(!owner)return JNI_FALSE;
    int flags=klj_extractor_sample_flags(owner.value);
    if(flags>=0&&(flags&2))fail(env,"java/lang/UnsupportedOperationException","Encrypted sample metadata is unavailable");
    return JNI_FALSE;
}
NATIVE(jlong,getCachedDuration)(JNIEnv *env,jobject self){Extractor owner(env,self);if(!owner)return -1;bool eos;return klj_extractor_cached_duration(owner.value,&eos);}
NATIVE(jboolean,hasCacheReachedEndOfStream)(JNIEnv *env,jobject self){Extractor owner(env,self);if(!owner)return JNI_TRUE;bool eos=true;int64_t duration=klj_extractor_cached_duration(owner.value,&eos);return duration<0||eos?JNI_TRUE:JNI_FALSE;}
NATIVE(void,native_1setLogSessionId)(JNIEnv *env,jobject self,jstring id){
    Extractor owner(env,self);if(!owner)return;
    if(!id){fail(env,"java/lang/NullPointerException");return;}
    Utf text(env,id);if(env->ExceptionCheck()||!text.chars)return;klj_extractor_log_session(owner.value,text.chars);
}
NATIVE(jobject,native_1getAudioPresentations)(JNIEnv *env,jobject self,jint index) {
    Extractor owner(env,self);if(!owner)return nullptr;
    if(index<0){fail(env,"java/lang/IllegalArgumentException");return nullptr;}
    auto metadata=format(owner.value,index);
    if(!metadata){fail(env,"java/lang/IllegalArgumentException");return nullptr;}
    try{return presentations(env,metadata.get());}
    catch(const std::bad_alloc &){allocationFailure(env);}
    catch(const std::ios_base::failure &){fail(env,"java/lang/IllegalArgumentException","Invalid audio presentation metadata");}
    return nullptr;
}
NATIVE(jobject,native_1getMetrics)(JNIEnv *env,jobject self) {
    Extractor owner(env,self);if(!owner)return nullptr;
    auto metadata=format(owner.value,-1);
    jobject bundle=newObject(env,"android/os/PersistableBundle");if(!bundle)return nullptr;
    Local<jclass> cls(env,env->GetObjectClass(bundle));if(env->ExceptionCheck())return nullptr;
    jmethodID putInt=env->GetMethodID(cls,"putInt","(Ljava/lang/String;I)V");if(env->ExceptionCheck()||!putInt)return nullptr;
    size_t count=klj_extractor_count(owner.value);
    if(count>INT_MAX){fail(env,"java/lang/IllegalStateException");return nullptr;}
    Local<jstring> countKey(env,env->NewStringUTF("android.media.mediaextractor.ntrk"));if(env->ExceptionCheck()||!countKey.value)return nullptr;
    env->CallVoidMethod(bundle,putInt,countKey.value,static_cast<jint>(count));if(env->ExceptionCheck())return nullptr;
    const char *mime=nullptr;
    if(metadata&&AMediaFormat_getString(metadata.get(),"mime",&mime)&&mime){
        jmethodID putString=env->GetMethodID(cls,"putString","(Ljava/lang/String;Ljava/lang/String;)V");if(env->ExceptionCheck()||!putString)return nullptr;
        Local<jstring> key(env,env->NewStringUTF("android.media.mediaextractor.mime"));if(env->ExceptionCheck()||!key.value)return nullptr;
        Local<jstring> value(env,env->NewStringUTF(mime));if(env->ExceptionCheck()||!value.value)return nullptr;
        env->CallVoidMethod(bundle,putString,key.value,value.value);if(env->ExceptionCheck())return nullptr;
    }
    return bundle;
}
#undef NATIVE
