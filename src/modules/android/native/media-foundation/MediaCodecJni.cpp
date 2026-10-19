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
#include "MediaCodecOwner.h"
#include "process-identity.h"
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <atomic>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <unistd.h>
#include <vector>

extern "C" AMediaFormat *klj_media_format_arrays(JNIEnv*,jobjectArray,jobjectArray);
extern "C" jobject klj_media_format_map(JNIEnv*,AMediaFormat*);
extern "C" int klj_codec_surface_java(JNIEnv*,jobject,KLJCodecSurface*);

namespace {
std::mutex contextMutex,initializationMutex;
JavaVM *javaVM=nullptr;
struct Fields {
    jfieldID context=nullptr,offset=nullptr,size=nullptr,time=nullptr,flags=nullptr;
    jclass bufferInfo=nullptr,byteBuffer=nullptr,codecException=nullptr;
    jobject byteOrder=nullptr;
    jmethodID post=nullptr,infoCtor=nullptr,infoSet=nullptr,exceptionCtor=nullptr;
    jmethodID asReadOnly=nullptr,order=nullptr;
} fields;
struct InitialFields {
    JNIEnv *env;Fields value;
    ~InitialFields(){
        if(value.bufferInfo)env->DeleteGlobalRef(value.bufferInfo);
        if(value.byteBuffer)env->DeleteGlobalRef(value.byteBuffer);
        if(value.codecException)env->DeleteGlobalRef(value.codecException);
        if(value.byteOrder)env->DeleteGlobalRef(value.byteOrder);
    }
    void commit(){fields=value;value={};}
};

template<class T> struct Local {
    JNIEnv *env;T value;
    Local(JNIEnv*e,T v):env(e),value(v){}
    ~Local(){if(value)env->DeleteLocalRef(value);}
    T take(){T out=value;value=nullptr;return out;}
};
struct Utf {
    JNIEnv *env;jstring value;const char *text;
    Utf(JNIEnv*e,jstring v):env(e),value(v),text(v?e->GetStringUTFChars(v,nullptr):nullptr){}
    ~Utf(){if(text)env->ReleaseStringUTFChars(value,text);}
};
using Format=std::unique_ptr<AMediaFormat,decltype(&AMediaFormat_delete)>;

void fail(JNIEnv *env,const char *type,const char *message){
    if(env->ExceptionCheck())return;
    Local<jclass> cls(env,env->FindClass(type));
    if(env->ExceptionCheck()||!cls.value)return;
    env->ThrowNew(cls.value,message);
}
template<class F> auto checked(JNIEnv *env,F body)->decltype(body()){
    try{return body();}
    catch(const std::bad_alloc&){fail(env,"java/lang/OutOfMemoryError","Codec allocation failed");}
    catch(...){fail(env,"java/lang/IllegalStateException","Native codec operation failed");}
    if constexpr(!std::is_void_v<decltype(body())>)return {};
}
jthrowable exception(JNIEnv *env,int status,const char *message){
    if(env->ExceptionCheck())return nullptr;
    Local<jstring> text(env,env->NewStringUTF(message));
    if(env->ExceptionCheck()||!text.value)return nullptr;
    jobject result=env->NewObject(fields.codecException,fields.exceptionCtor,static_cast<jint>(status),0,text.value);
    return env->ExceptionCheck()?nullptr:static_cast<jthrowable>(result);
}
bool result(JNIEnv *env,int status){
    if(env->ExceptionCheck())return false;
    if(status==AMEDIA_OK)return true;
    if(status==AMEDIA_ERROR_INVALID_OPERATION||status==AMEDIA_ERROR_INVALID_OBJECT)
        fail(env,"java/lang/IllegalStateException","Codec is not in the required state");
    else if(status==AMEDIA_ERROR_INVALID_PARAMETER)
        fail(env,"java/lang/IllegalArgumentException","Invalid codec argument");
    else{
        Local<jthrowable> value(env,exception(env,status,status==AMEDIA_ERROR_UNSUPPORTED?"Codec operation is not supported":"Codec operation failed"));
        if(value.value&&!env->ExceptionCheck())env->Throw(value.value);
    }
    return false;
}
struct Codec {
    KLJCodec *value=nullptr;
    Codec(JNIEnv *env,jobject self){
        {std::lock_guard lock(contextMutex);
            if(fields.context)value=reinterpret_cast<KLJCodec*>(env->GetLongField(self,fields.context));
            if(env->ExceptionCheck())value=nullptr;
            if(value)klj_codec_retain(value);
        }
        if(!value)fail(env,"java/lang/IllegalStateException","Codec is released");
    }
    ~Codec(){if(value)klj_codec_release(value);}
};
template<class F> void operation(JNIEnv *env,jobject self,F body){checked(env,[&]{Codec codec(env,self);if(codec.value)result(env,body(codec.value));});}
struct Surface {
    KLJCodecSurface value={};
    ~Surface(){if(value.release)value.release(value.context);}
};

// Native workers attach once for their thread lifetime. Java-originating
// threads remain borrowed and are never detached by this adapter.
struct Attachment {
    JavaVM *vm=nullptr;JNIEnv *env=nullptr;bool own=false;
    JNIEnv *get(){
        if(env)return env;
        vm=javaVM;if(!vm)return nullptr;
        jint code=vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6);
        if(code==JNI_EDETACHED){if(vm->AttachCurrentThreadAsDaemon(&env,nullptr)!=JNI_OK)env=nullptr;else own=true;}
        else if(code!=JNI_OK)env=nullptr;
        return env;
    }
    ~Attachment(){if(own)vm->DetachCurrentThread();}
};
JNIEnv *callbackEnv(){thread_local Attachment value;return value.get();}
void reportCallbackException(JNIEnv *env){
    if(!env->ExceptionCheck())return;
    Local<jthrowable> error(env,env->ExceptionOccurred());env->ExceptionClear();
    Local<jclass> threadClass(env,env->FindClass("java/lang/Thread"));
    if(env->ExceptionCheck()||!threadClass.value){env->ExceptionClear();return;}
    auto current=env->GetStaticMethodID(threadClass.value,"currentThread","()Ljava/lang/Thread;");
    if(env->ExceptionCheck()||!current){env->ExceptionClear();return;}
    Local<jobject> thread(env,env->CallStaticObjectMethod(threadClass.value,current));
    if(env->ExceptionCheck()||!thread.value){env->ExceptionClear();return;}
    auto get=env->GetMethodID(threadClass.value,"getUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;");
    if(env->ExceptionCheck()||!get){env->ExceptionClear();return;}
    Local<jobject> handler(env,env->CallObjectMethod(thread.value,get));
    if(env->ExceptionCheck()||!handler.value){env->ExceptionClear();return;}
    Local<jclass> cls(env,env->GetObjectClass(handler.value));
    if(env->ExceptionCheck()||!cls.value){env->ExceptionClear();return;}
    auto dispatch=env->GetMethodID(cls.value,"uncaughtException","(Ljava/lang/Thread;Ljava/lang/Throwable;)V");
    if(!env->ExceptionCheck()&&dispatch)env->CallVoidMethod(handler.value,dispatch,thread.value,error.value);
    if(env->ExceptionCheck())env->ExceptionClear();
}
struct JavaCallbacks {
    std::atomic<unsigned> references{1};jweak object=nullptr;
    JavaCallbacks(JNIEnv *env,jobject self):object(env->NewWeakGlobalRef(self)){}
    ~JavaCallbacks(){if(object)if(auto*env=callbackEnv())env->DeleteWeakGlobalRef(object);}
    static void retain(void*p){static_cast<JavaCallbacks*>(p)->references.fetch_add(1,std::memory_order_relaxed);}
    static void release(void*p){auto*s=static_cast<JavaCallbacks*>(p);if(s->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete s;}
    template<class F> void post(int event,int first,int second,F make){
        JNIEnv *env=callbackEnv();if(!env)return;
        checked(env,[&]{
            Local<jobject> self(env,env->NewLocalRef(object));if(env->ExceptionCheck()||!self.value)return;
            Local<jobject> payload(env,make(env));if(env->ExceptionCheck())return;
            env->CallVoidMethod(self.value,fields.post,event,first,second,payload.value);
        });
        reportCallbackException(env);
    }
    static void input(void*p,size_t index){
        auto*s=static_cast<JavaCallbacks*>(p);
        if(index>INT_MAX){error(p,AMEDIA_ERROR_INVALID_PARAMETER);return;}
        s->post(1,1,static_cast<int>(index),[](JNIEnv*)->jobject{return nullptr;});
    }
    static void output(void*p,size_t index,const KLJCodecBufferInfo *info){
        auto*s=static_cast<JavaCallbacks*>(p);
        if(index>INT_MAX){error(p,AMEDIA_ERROR_INVALID_PARAMETER);return;}
        s->post(1,2,static_cast<int>(index),[&](JNIEnv*env)->jobject{
            Local<jobject> value(env,env->NewObject(fields.bufferInfo,fields.infoCtor));
            if(env->ExceptionCheck()||!value.value)return nullptr;
            env->CallVoidMethod(value.value,fields.infoSet,info->offset,info->size,static_cast<jlong>(info->presentationTimeUs),static_cast<jint>(info->flags));
            return env->ExceptionCheck()?nullptr:value.take();
        });
    }
    static void format(void*p,const AMediaFormat *format){static_cast<JavaCallbacks*>(p)->post(1,4,0,[&](JNIEnv*env){return klj_media_format_map(env,const_cast<AMediaFormat*>(format));});}
    static void error(void*p,int status){static_cast<JavaCallbacks*>(p)->post(1,3,0,[&](JNIEnv*env)->jobject{return exception(env,status,"Native decoder error");});}
    static void rendered(void*p,int64_t media,int64_t system){
        static_cast<JavaCallbacks*>(p)->post(3,0,0,[&](JNIEnv*env)->jobject{
            Format data(AMediaFormat_new(),AMediaFormat_delete);
            if(!data){fail(env,"java/lang/OutOfMemoryError","Frame metadata allocation failed");return nullptr;}
            AMediaFormat_setInt64(data.get(),"0-media-time-us",media);
            AMediaFormat_setInt64(data.get(),"0-system-nano",system);
            return klj_media_format_map(env,data.get());
        });
    }
};

jobject byteBuffer(JNIEnv *env,KLJCodec *codec,bool input,size_t index,bool array){
    size_t capacity=0;
    uint8_t *bytes=array?klj_codec_array_buffer(codec,input,index,&capacity):input?klj_codec_input_buffer(codec,index,&capacity):klj_codec_output_buffer(codec,index,&capacity);
    if(!bytes)return nullptr;
    if(capacity>INT_MAX){fail(env,"java/lang/IllegalArgumentException","Codec buffer exceeds Java capacity");return nullptr;}
    // The owner's non-array output view is exactly the current BufferInfo
    // range (offset zero), including native row padding. NewDirectByteBuffer
    // therefore gives the original position=0, limit=current size semantics.
    Local<jobject> buffer(env,env->NewDirectByteBuffer(bytes,static_cast<jlong>(capacity)));
    if(env->ExceptionCheck()||!buffer.value)return nullptr;
    if(!input){
        jobject readOnly=env->CallObjectMethod(buffer.value,fields.asReadOnly);
        if(env->ExceptionCheck())return nullptr;
        env->DeleteLocalRef(buffer.value);buffer.value=readOnly;
        if(!buffer.value)return nullptr;
    }
    Local<jobject> ordered(env,env->CallObjectMethod(buffer.value,fields.order,fields.byteOrder));
    if(env->ExceptionCheck())return nullptr;
    return buffer.take();
}
void unsupported(JNIEnv *env,const char *message){fail(env,"java/lang/UnsupportedOperationException",message);}
void unavailable(JNIEnv *env,jobject self,const char *message){Codec codec(env,self);if(codec.value)unsupported(env,message);}
void retire(KLJCodec *value){
    std::unique_ptr<KLJCodec,decltype(&klj_codec_release)> retained(value,klj_codec_release);
    if(value)klj_codec_close(value);
}
void release(JNIEnv *env,jobject self){
    KLJCodec *old=nullptr;
    {std::lock_guard lock(contextMutex);
        if(!fields.context)return;
        old=reinterpret_cast<KLJCodec*>(env->GetLongField(self,fields.context));if(env->ExceptionCheck())return;
        env->SetLongField(self,fields.context,0);if(env->ExceptionCheck())return;
    }
    retire(old);
}
// VideoToolbox exposes no Android vendor-extension parameters. Original
// subscription semantics ignore unknown names, while Java argument failures
// remain visible. There is no synthetic vendor parameter registry.
void vendorSubscription(JNIEnv *env,jobject self,jobject names){checked(env,[&]{
    Codec codec(env,self);if(!codec.value)return;
    if(!names){fail(env,"java/lang/NullPointerException","Parameter names are null");return;}
    Local<jclass> list(env,env->FindClass("java/util/List"));if(env->ExceptionCheck()||!list.value)return;
    auto iterator=env->GetMethodID(list.value,"iterator","()Ljava/util/Iterator;");if(env->ExceptionCheck()||!iterator)return;
    Local<jobject> it(env,env->CallObjectMethod(names,iterator));if(env->ExceptionCheck()||!it.value)return;
    Local<jclass> iteratorClass(env,env->FindClass("java/util/Iterator"));if(env->ExceptionCheck()||!iteratorClass.value)return;
    auto hasNext=env->GetMethodID(iteratorClass.value,"hasNext","()Z");if(env->ExceptionCheck()||!hasNext)return;
    auto next=env->GetMethodID(iteratorClass.value,"next","()Ljava/lang/Object;");if(env->ExceptionCheck()||!next)return;
    Local<jclass> string(env,env->FindClass("java/lang/String"));if(env->ExceptionCheck()||!string.value)return;
    for(;;){
        bool more=env->CallBooleanMethod(it.value,hasNext);if(env->ExceptionCheck()||!more)return;
        Local<jobject> name(env,env->CallObjectMethod(it.value,next));if(env->ExceptionCheck())return;
        if(!name.value){fail(env,"java/lang/NullPointerException","Parameter name is null");return;}
        bool valid=env->IsInstanceOf(name.value,string.value);if(env->ExceptionCheck())return;
        if(!valid){fail(env,"java/lang/IllegalArgumentException","Parameter name is not a string");return;}
    }
});}
}

extern "C" {
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1init(JNIEnv *env,jclass cls){checked(env,[&]{
    std::lock_guard lock(initializationMutex);if(fields.context)return;
    if(env->GetJavaVM(&javaVM)!=JNI_OK){fail(env,"java/lang/IllegalStateException","Java runtime is unavailable");return;}
    InitialFields pending{env,{}};Fields &next=pending.value;
#define METHOD(dst,type,name,signature) next.dst=env->GetMethodID(type,name,signature);if(env->ExceptionCheck()||!next.dst)return
#define FIELD(dst,type,name,signature) next.dst=env->GetFieldID(type,name,signature);if(env->ExceptionCheck()||!next.dst)return
#define CLASS(dst,name) {Local<jclass> local(env,env->FindClass(name));if(env->ExceptionCheck()||!local.value)return;next.dst=static_cast<jclass>(env->NewGlobalRef(local.value));if(env->ExceptionCheck()||!next.dst)return;}
    FIELD(context,cls,"mNativeContext","J");METHOD(post,cls,"postEventFromNative","(IIILjava/lang/Object;)V");
    CLASS(bufferInfo,"android/media/MediaCodec$BufferInfo");
    METHOD(infoCtor,next.bufferInfo,"<init>","()V");METHOD(infoSet,next.bufferInfo,"set","(IIJI)V");
    FIELD(offset,next.bufferInfo,"offset","I");FIELD(size,next.bufferInfo,"size","I");FIELD(time,next.bufferInfo,"presentationTimeUs","J");FIELD(flags,next.bufferInfo,"flags","I");
    CLASS(codecException,"android/media/MediaCodec$CodecException");METHOD(exceptionCtor,next.codecException,"<init>","(IILjava/lang/String;)V");
    CLASS(byteBuffer,"java/nio/ByteBuffer");METHOD(asReadOnly,next.byteBuffer,"asReadOnlyBuffer","()Ljava/nio/ByteBuffer;");METHOD(order,next.byteBuffer,"order","(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;");
    Local<jclass> order(env,env->FindClass("java/nio/ByteOrder"));if(env->ExceptionCheck()||!order.value)return;
    auto native=env->GetStaticMethodID(order.value,"nativeOrder","()Ljava/nio/ByteOrder;");if(env->ExceptionCheck()||!native)return;
    Local<jobject> value(env,env->CallStaticObjectMethod(order.value,native));if(env->ExceptionCheck()||!value.value)return;
    next.byteOrder=env->NewGlobalRef(value.value);if(env->ExceptionCheck()||!next.byteOrder)return;
    pending.commit();
#undef METHOD
#undef FIELD
#undef CLASS
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1setup(JNIEnv *env,jobject self,jstring name,jboolean type,jboolean encoder,jint pid,jint uid){checked(env,[&]{
    if(!name){fail(env,"java/lang/NullPointerException","Codec name is null");return;}
    if((pid!=-1&&pid!=getpid())||(uid!=-1&&static_cast<uid_t>(uid)!=klj_process_uid())){fail(env,"java/lang/SecurityException","Codec client identity is not the calling process");return;}
    Utf text(env,name);if(env->ExceptionCheck()||!text.text)return;
    KLJCodec *next=klj_codec_create(text.text,type,encoder);
    if(!next){fail(env,"java/lang/IllegalArgumentException","No decoder supports the requested codec");return;}
    KLJCodec *old=nullptr;
    {std::lock_guard lock(contextMutex);old=reinterpret_cast<KLJCodec*>(env->GetLongField(self,fields.context));if(!env->ExceptionCheck())env->SetLongField(self,fields.context,reinterpret_cast<jlong>(next));}
    if(env->ExceptionCheck()){retire(next);return;}
    retire(old);
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1release(JNIEnv *env,jobject self){checked(env,[&]{release(env,self);});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1finalize(JNIEnv *env,jobject self){checked(env,[&]{release(env,self);});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1reset(JNIEnv *env,jobject self){operation(env,self,klj_codec_reset);}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1start(JNIEnv *env,jobject self){operation(env,self,klj_codec_start);}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1stop(JNIEnv *env,jobject self){operation(env,self,klj_codec_stop);}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1flush(JNIEnv *env,jobject self){operation(env,self,klj_codec_flush);}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1configure(JNIEnv *env,jobject self,jobjectArray keys,jobjectArray values,jobject window,jobject crypto,jobject descrambler,jint flags){checked(env,[&]{
    Codec codec(env,self);if(!codec.value)return;
    if(crypto||descrambler){result(env,AMEDIA_ERROR_UNSUPPORTED);return;}
    Format format(klj_media_format_arrays(env,keys,values),AMediaFormat_delete);if(env->ExceptionCheck()||!format)return;
    Surface surface;if(window){int code=klj_codec_surface_java(env,window,&surface.value);if(!result(env,code))return;}
    result(env,klj_codec_configure(codec.value,format.get(),window?&surface.value:nullptr,static_cast<unsigned>(flags)));
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1setSurface(JNIEnv *env,jobject self,jobject window){checked(env,[&]{
    Codec codec(env,self);if(!codec.value)return;
    Surface surface;if(window){int code=klj_codec_surface_java(env,window,&surface.value);if(!result(env,code))return;}
    result(env,klj_codec_set_surface(codec.value,window?&surface.value:nullptr));
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1detachOutputSurface(JNIEnv *env,jobject self){operation(env,self,[](KLJCodec*c){return klj_codec_set_surface(c,nullptr);});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_setParameters(JNIEnv *env,jobject self,jobjectArray keys,jobjectArray values){checked(env,[&]{
    Codec codec(env,self);if(!codec.value)return;Format format(klj_media_format_arrays(env,keys,values),AMediaFormat_delete);
    if(!env->ExceptionCheck()&&format)result(env,klj_codec_parameters(codec.value,format.get()));
});}
JNIEXPORT jint JNICALL Java_android_media_MediaCodec_native_1dequeueInputBuffer(JNIEnv *env,jobject self,jlong timeout){return checked(env,[&]()->jint{
    Codec codec(env,self);if(!codec.value)return 0;ssize_t value=klj_codec_dequeue_input(codec.value,timeout);
    if(value>=0&&value<=INT_MAX)return static_cast<jint>(value);
    if(value==AMEDIACODEC_INFO_TRY_AGAIN_LATER)return -1;
    result(env,value>INT_MAX?AMEDIA_ERROR_INVALID_PARAMETER:static_cast<int>(value));return 0;
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1queueInputBuffer(JNIEnv *env,jobject self,jint index,jint offset,jint size,jlong time,jint flags){operation(env,self,[&](KLJCodec*c){
    return index<0||offset<0||size<0?AMEDIA_ERROR_INVALID_PARAMETER:klj_codec_queue_input(c,static_cast<size_t>(index),offset,static_cast<size_t>(size),time,static_cast<unsigned>(flags));
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1queueInputBuffers(JNIEnv *env,jobject self,jint index,jobjectArray values){checked(env,[&]{
    Codec codec(env,self);if(!codec.value)return;if(index<0||!values){result(env,AMEDIA_ERROR_INVALID_PARAMETER);return;}
    jsize count=env->GetArrayLength(values);if(env->ExceptionCheck())return;if(count<=0){result(env,AMEDIA_ERROR_INVALID_PARAMETER);return;}
    std::vector<KLJCodecBufferInfo> infos;infos.reserve(static_cast<size_t>(count));int64_t end=0;
    for(jsize i=0;i<count;i++){
        Local<jobject> info(env,env->GetObjectArrayElement(values,i));if(env->ExceptionCheck())return;
        if(!info.value||!env->IsInstanceOf(info.value,fields.bufferInfo)){result(env,AMEDIA_ERROR_INVALID_PARAMETER);return;}
        KLJCodecBufferInfo value={};
        value.offset=env->GetIntField(info.value,fields.offset);if(env->ExceptionCheck())return;
        value.size=env->GetIntField(info.value,fields.size);if(env->ExceptionCheck())return;
        value.presentationTimeUs=env->GetLongField(info.value,fields.time);if(env->ExceptionCheck())return;
        value.flags=static_cast<uint32_t>(env->GetIntField(info.value,fields.flags));if(env->ExceptionCheck())return;
        if(value.offset<0||value.size<0||static_cast<int64_t>(value.offset)+value.size>INT_MAX||(i&&value.offset!=end)||(!value.flags&&!value.size)){result(env,AMEDIA_ERROR_INVALID_PARAMETER);return;}
        end=static_cast<int64_t>(value.offset)+value.size;infos.push_back(value);
    }
    result(env,klj_codec_queue_inputs(codec.value,static_cast<size_t>(index),infos.data(),infos.size()));
});}
JNIEXPORT jint JNICALL Java_android_media_MediaCodec_native_1dequeueOutputBuffer(JNIEnv *env,jobject self,jobject info,jlong timeout){return checked(env,[&]()->jint{
    Codec codec(env,self);if(!codec.value)return 0;if(!info){fail(env,"java/lang/NullPointerException","BufferInfo is null");return 0;}
    KLJCodecBufferInfo native={};ssize_t value=klj_codec_dequeue_output(codec.value,&native,timeout);
    if(value>=0&&value<=INT_MAX){env->CallVoidMethod(info,fields.infoSet,native.offset,native.size,static_cast<jlong>(native.presentationTimeUs),static_cast<jint>(native.flags));if(env->ExceptionCheck())return 0;return static_cast<jint>(value);}
    if(value>=-3&&value<=-1)return static_cast<jint>(value);
    result(env,value>INT_MAX?AMEDIA_ERROR_INVALID_PARAMETER:static_cast<int>(value));return 0;
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_releaseOutputBuffer(JNIEnv *env,jobject self,jint index,jboolean render,jboolean updateTime,jlong time){operation(env,self,[&](KLJCodec*c){return index<0?AMEDIA_ERROR_INVALID_PARAMETER:klj_codec_release_output(c,static_cast<size_t>(index),render,updateTime?time:-1);});}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_getBuffer(JNIEnv *env,jobject self,jboolean input,jint index){return checked(env,[&]()->jobject{
    Codec codec(env,self);if(!codec.value)return nullptr;if(index<0){result(env,AMEDIA_ERROR_INVALID_PARAMETER);return nullptr;}return byteBuffer(env,codec.value,input,static_cast<size_t>(index),false);
});}
JNIEXPORT jobjectArray JNICALL Java_android_media_MediaCodec_getBuffers(JNIEnv *env,jobject self,jboolean input){return checked(env,[&]()->jobjectArray{
    Codec codec(env,self);if(!codec.value)return nullptr;size_t count=input?klj_codec_input_count(codec.value):klj_codec_output_count(codec.value);
    if(count>INT_MAX){result(env,AMEDIA_ERROR_INVALID_PARAMETER);return nullptr;}
    Local<jobjectArray> values(env,env->NewObjectArray(static_cast<jsize>(count),fields.byteBuffer,nullptr));if(env->ExceptionCheck()||!values.value)return nullptr;
    for(size_t i=0;i<count;i++){Local<jobject> value(env,byteBuffer(env,codec.value,input,i,true));if(env->ExceptionCheck())return nullptr;env->SetObjectArrayElement(values.value,static_cast<jsize>(i),value.value);if(env->ExceptionCheck())return nullptr;}
    return values.take();
});}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_getFormatNative(JNIEnv *env,jobject self,jboolean input){return checked(env,[&]()->jobject{
    Codec codec(env,self);if(!codec.value)return nullptr;Format format(klj_codec_format(codec.value,input),AMediaFormat_delete);
    if(!format){result(env,AMEDIA_ERROR_INVALID_OPERATION);return nullptr;}return klj_media_format_map(env,format.get());
});}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_getOutputFormatNative(JNIEnv *env,jobject self,jint index){return checked(env,[&]()->jobject{
    Codec codec(env,self);if(!codec.value)return nullptr;if(index<0){result(env,AMEDIA_ERROR_INVALID_PARAMETER);return nullptr;}
    Format format(klj_codec_buffer_format(codec.value,static_cast<size_t>(index)),AMediaFormat_delete);if(!format){result(env,AMEDIA_ERROR_INVALID_OPERATION);return nullptr;}return klj_media_format_map(env,format.get());
});}
JNIEXPORT jstring JNICALL Java_android_media_MediaCodec_getCanonicalName(JNIEnv *env,jobject self){return checked(env,[&]()->jstring{Codec codec(env,self);if(!codec.value)return nullptr;const char*name=klj_codec_name(codec.value);return name?env->NewStringUTF(name):nullptr;});}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_native_1getMetrics(JNIEnv *env,jobject self){return checked(env,[&]()->jobject{
    Codec codec(env,self);if(!codec.value)return nullptr;
    Local<jclass> cls(env,env->FindClass("android/os/PersistableBundle"));if(env->ExceptionCheck()||!cls.value)return nullptr;
    auto ctor=env->GetMethodID(cls.value,"<init>","()V");if(env->ExceptionCheck()||!ctor)return nullptr;
    auto putString=env->GetMethodID(cls.value,"putString","(Ljava/lang/String;Ljava/lang/String;)V");if(env->ExceptionCheck()||!putString)return nullptr;
    auto putInt=env->GetMethodID(cls.value,"putInt","(Ljava/lang/String;I)V");if(env->ExceptionCheck()||!putInt)return nullptr;
    Local<jobject> bundle(env,env->NewObject(cls.value,ctor));if(env->ExceptionCheck()||!bundle.value)return nullptr;
    auto string=[&](const char *key,const char *value){
        if(!value||env->ExceptionCheck())return;
        Local<jstring> k(env,env->NewStringUTF(key));if(env->ExceptionCheck()||!k.value)return;
        Local<jstring> v(env,env->NewStringUTF(value));if(env->ExceptionCheck()||!v.value)return;
        env->CallVoidMethod(bundle.value,putString,k.value,v.value);
    };
    auto integer=[&](const char *key,int32_t value){
        if(env->ExceptionCheck())return;
        Local<jstring> k(env,env->NewStringUTF(key));if(env->ExceptionCheck()||!k.value)return;
        env->CallVoidMethod(bundle.value,putInt,k.value,static_cast<jint>(value));
    };
    string("android.media.mediacodec.codec",klj_codec_name(codec.value));
    // This native owner creates only clear-content decoders. No timing,
    // frame-count or resource statistics are invented by the JNI adapter.
    integer("android.media.mediacodec.encoder",0);
    integer("android.media.mediacodec.secure",0);
    Format format(klj_codec_format(codec.value,false),AMediaFormat_delete);
    if(!format)format.reset(klj_codec_format(codec.value,true));
    if(format){
        const char *mime=nullptr;
        if(AMediaFormat_getString(format.get(),AMEDIAFORMAT_KEY_MIME,&mime)){
            string("android.media.mediacodec.mime",mime);
            if(!std::strncmp(mime,"video/",6))string("android.media.mediacodec.mode","video");
            else if(!std::strncmp(mime,"audio/",6))string("android.media.mediacodec.mode","audio");
        }
        for(const auto &entry:{std::pair{"width","android.media.mediacodec.width"},std::pair{"height","android.media.mediacodec.height"},std::pair{"rotation-degrees","android.media.mediacodec.rotation"}}){
            int32_t value=0;if(AMediaFormat_getInt32(format.get(),entry.first,&value))integer(entry.second,value);
        }
    }
    return env->ExceptionCheck()?nullptr:bundle.take();
});}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_native_1getSupportedVendorParameters(JNIEnv *env,jobject self){return checked(env,[&]()->jobject{
    Codec codec(env,self);if(!codec.value)return nullptr;
    Local<jclass> cls(env,env->FindClass("java/util/ArrayList"));if(env->ExceptionCheck()||!cls.value)return nullptr;
    auto ctor=env->GetMethodID(cls.value,"<init>","()V");if(env->ExceptionCheck()||!ctor)return nullptr;
    return env->NewObject(cls.value,ctor);
});}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_native_1getParameterDescriptor(JNIEnv *env,jobject self,jstring name){return checked(env,[&]()->jobject{
    Codec codec(env,self);if(!codec.value)return nullptr;
    if(!name)fail(env,"java/lang/NullPointerException","Parameter name is null");
    return nullptr; // The native backend exposes no Android vendor extensions.
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1subscribeToVendorParameters(JNIEnv *env,jobject self,jobject names){vendorSubscription(env,self,names);}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1unsubscribeFromVendorParameters(JNIEnv *env,jobject self,jobject names){vendorSubscription(env,self,names);}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1setCallback(JNIEnv *env,jobject self,jobject callback){checked(env,[&]{
    Codec codec(env,self);if(!codec.value)return;
    if(!callback){result(env,klj_codec_callbacks(codec.value,nullptr));return;}
    auto*state=new JavaCallbacks(env,self);if(env->ExceptionCheck()||!state->object){JavaCallbacks::release(state);return;}
    KLJCodecCallbacks bridge={state,JavaCallbacks::retain,JavaCallbacks::release,JavaCallbacks::input,JavaCallbacks::output,JavaCallbacks::format,JavaCallbacks::error};
    int code;
    try{code=klj_codec_callbacks(codec.value,&bridge);}catch(...){JavaCallbacks::release(state);throw;}
    JavaCallbacks::release(state);result(env,code);
});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1enableOnFrameRenderedListener(JNIEnv *env,jobject self,jboolean enabled){checked(env,[&]{
    Codec codec(env,self);if(!codec.value)return;
    if(!enabled){result(env,klj_codec_rendered_callbacks(codec.value,nullptr));return;}
    auto*state=new JavaCallbacks(env,self);if(env->ExceptionCheck()||!state->object){JavaCallbacks::release(state);return;}
    KLJCodecRenderedCallbacks bridge={state,JavaCallbacks::retain,JavaCallbacks::release,JavaCallbacks::rendered};
    int code;
    try{code=klj_codec_rendered_callbacks(codec.value,&bridge);}catch(...){JavaCallbacks::release(state);throw;}
    JavaCallbacks::release(state);result(env,code);
});}

// These entrypoints remain available. The current decoder owner has no secure
// session, encoder, Codec2 block allocator, image-plane view or tunnel owner.
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1queueSecureInputBuffer(JNIEnv *env,jobject self,jint,jint,jobject,jlong,jint){unavailable(env,self,"Secure decoding is not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1queueSecureInputBuffers(JNIEnv *env,jobject self,jint,jobjectArray,jobjectArray){unavailable(env,self,"Secure decoding is not supported");}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_createInputSurface(JNIEnv *env,jobject self){unavailable(env,self,"Surface input encoding is not supported");return nullptr;}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_native_1createPersistentInputSurface(JNIEnv *env,jclass){unsupported(env,"Persistent encoder surfaces are not supported");return nullptr;}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1releasePersistentInputSurface(JNIEnv *env,jclass,jobject){unsupported(env,"Persistent encoder surfaces are not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1setInputSurface(JNIEnv *env,jobject self,jobject){unavailable(env,self,"Surface input encoding is not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_signalEndOfInputStream(JNIEnv *env,jobject self){operation(env,self,[](KLJCodec*){return AMEDIA_ERROR_INVALID_OPERATION;});}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1enableOnFirstTunnelFrameReadyListener(JNIEnv *env,jobject self,jboolean enabled){Codec codec(env,self);if(codec.value&&enabled)unsupported(env,"Tunneled decoding is not supported");}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_getImage(JNIEnv *env,jobject self,jboolean,jint){unavailable(env,self,"Codec image-plane access is not supported");return nullptr;}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_native_1mapHardwareBuffer(JNIEnv *env,jclass,jobject){unsupported(env,"Codec hardware-buffer image mapping is not supported");return nullptr;}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1closeMediaImage(JNIEnv *env,jclass,jlong value){if(value)unsupported(env,"Codec hardware-buffer image mapping is not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1queueLinearBlock(JNIEnv *env,jobject self,jint,jobject,jobjectArray,jobjectArray,jobject,jobject){unavailable(env,self,"Codec2 linear-block input is not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1queueHardwareBuffer(JNIEnv *env,jobject self,jint,jobject,jlong,jint,jobject,jobject){unavailable(env,self,"Hardware-buffer encoding is not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1getOutputFrame(JNIEnv *env,jobject self,jobject,jint){unavailable(env,self,"Codec2 output blocks are not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_setVideoScalingMode(JNIEnv *env,jobject self,jint mode){if(mode!=1&&mode!=2){result(env,AMEDIA_ERROR_INVALID_PARAMETER);return;}unavailable(env,self,"Codec surface scaling control is not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1setAudioPresentation(JNIEnv *env,jobject self,jint,jint){unavailable(env,self,"This codec does not decode audio presentations");}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_getOwnCodecInfo(JNIEnv *env,jobject self){return checked(env,[&]()->jobject{
    Codec codec(env,self);if(!codec.value)return nullptr;
    Local<jclass> cls(env,env->FindClass("android/media/MediaCodecList"));if(!cls.value)return nullptr;
    auto method=env->GetStaticMethodID(cls.value,"getInfoFor","(Ljava/lang/String;)Landroid/media/MediaCodecInfo;");if(!method)return nullptr;
    Local<jstring> name(env,env->NewStringUTF(klj_codec_name(codec.value)));if(!name.value)return nullptr;
    return env->CallStaticObjectMethod(cls.value,method,name.value);
});}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_native_1getGloballyAvailableResources(JNIEnv *env,jclass){unsupported(env,"Global codec resource accounting is not available");return nullptr;}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_native_1getRequiredResources(JNIEnv *env,jobject self){unavailable(env,self,"Codec resource accounting is not available");return nullptr;}
JNIEXPORT jobject JNICALL Java_android_media_MediaCodec_00024LinearBlock_native_1map(JNIEnv *env,jobject){unsupported(env,"Codec2 linear-block allocation is not supported");return nullptr;}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_00024LinearBlock_native_1recycle(JNIEnv *env,jobject){unsupported(env,"Codec2 linear-block allocation is not supported");}
JNIEXPORT void JNICALL Java_android_media_MediaCodec_00024LinearBlock_native_1obtain(JNIEnv *env,jobject,jint,jobjectArray){unsupported(env,"Codec2 linear-block allocation is not supported");}
JNIEXPORT jboolean JNICALL Java_android_media_MediaCodec_00024LinearBlock_native_1checkCompatible(JNIEnv *env,jclass,jobjectArray){unsupported(env,"Codec2 linear-block allocation is not supported");return JNI_FALSE;}
}
