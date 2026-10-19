#include <jni.h>
#include <climits>
#include <utility>
#include "webkit-binding.h"

namespace klj::web {
namespace {
struct Method {const char*key;const char*owner;const char*name;const char*descriptor;bool isStatic;};
const Method methods[]={
#include "webkit-callbacks.inc"
};
const Method& method(const char*key){for(const auto&row:methods)if(!strcmp(row.key,key))return row;throw std::runtime_error("Browser callback descriptor is missing");}
void fail(JNIEnv*e,const char*message){if(e->ExceptionCheck())return;jclass type=e->FindClass("java/lang/IllegalStateException");if(type){e->ThrowNew(type,message);e->DeleteLocalRef(type);}}
Value value(JNIEnv*e,jstring s){if(!s)return {};const jchar*p=e->GetStringChars(s,nullptr);if(!p)return {};std::u16string v(reinterpret_cast<const char16_t*>(p),e->GetStringLength(s));e->ReleaseStringChars(s,p);return Value(std::move(v));}
Value value(JNIEnv*e,jbyteArray a){if(!a)return {};std::vector<uint8_t>v(e->GetArrayLength(a));e->GetByteArrayRegion(a,0,v.size(),reinterpret_cast<jbyte*>(v.data()));return Value(std::move(v));}
Value value(JNIEnv*e,jobjectArray a){Args v;if(a)for(jsize i=0;i<e->GetArrayLength(a);i++){auto s=static_cast<jstring>(e->GetObjectArrayElement(a,i));v.push_back(value(e,s));e->DeleteLocalRef(s);}return Value(std::move(v));}
jobject object(JNIEnv*e,const Value&v,bool direct=false){
 switch(v.type){
 case Value::null:return nullptr;
 case Value::text:return e->NewString(reinterpret_cast<const jchar*>(v.string.data()),v.string.size());
 case Value::bytes:if(direct)return e->NewDirectByteBuffer(const_cast<uint8_t*>(v.bytesData()),v.bytesSize());else{if(v.bytesSize()>INT_MAX)throw std::runtime_error("Browser data exceeds the Java array size");jbyteArray a=e->NewByteArray(v.bytesSize());if(a)e->SetByteArrayRegion(a,0,v.bytesSize(),reinterpret_cast<const jbyte*>(v.bytesData()));return a;}
 case Value::list:{jclass type=e->FindClass("java/lang/String");if(!type)return nullptr;jobjectArray a=e->NewObjectArray(v.items.size(),type,nullptr);e->DeleteLocalRef(type);for(size_t i=0;a&&i<v.items.size()&&!e->ExceptionCheck();i++){auto s=object(e,v.items[i]);e->SetObjectArrayElement(a,i,s);e->DeleteLocalRef(s);}return a;}
 default:throw std::runtime_error("Browser callback object type is invalid");
 }
}
struct JniCallback final:Callback {
 JavaVM*vm=nullptr;jobject receiver=nullptr,frameBuffer=nullptr;jclass receiverType=nullptr,failures=nullptr;jmethodID report=nullptr;
 const void*frameAddress=nullptr;size_t frameBytes=0;
 JniCallback(JNIEnv*e,jobject owner){e->GetJavaVM(&vm);receiver=e->NewGlobalRef(owner);auto type=e->GetObjectClass(owner);receiverType=static_cast<jclass>(e->NewGlobalRef(type));e->DeleteLocalRef(type);const auto&m=method("failure_report");type=e->FindClass(m.owner);if(type){failures=static_cast<jclass>(e->NewGlobalRef(type));e->DeleteLocalRef(type);report=e->GetStaticMethodID(failures,m.name,m.descriptor);}}
 JNIEnv*env(){JNIEnv*e=nullptr;return vm->GetEnv(reinterpret_cast<void**>(&e),JNI_VERSION_1_6)==JNI_OK?e:nullptr;}
 ~JniCallback(){if(auto e=env()){e->DeleteGlobalRef(frameBuffer);e->DeleteGlobalRef(receiver);e->DeleteGlobalRef(receiverType);e->DeleteGlobalRef(failures);}}
 Value invoke(Event event,const Args&a)override{
  auto lifetime=shared_from_this();
  JNIEnv*e=env();if(!e)throw std::runtime_error("Browser callback requires its VM thread");
  const char*key=nullptr;switch(event){
   case Event::changed:key="web_changed";break;case Event::dimensions:key="web_dimensions";break;
   case Event::navigation:key="web_navigation";break;case Event::history:key="web_history";break;
   case Event::dialog:key="web_dialog";break;case Event::resource:key="web_resource";break;
   case Event::editor:key="web_editor";break;case Event::port:key="web_port";break;
   case Event::navigationRequest:key="web_navigation_request";break;case Event::interface:key="web_interface";break;
   case Event::script:key="web_script";break;case Event::frame:key="web_frame";break;case Event::result:key="web_result";break;
  }
  const auto&m=method(key);jmethodID id=e->GetMethodID(receiverType,m.name,m.descriptor);if(!id)return {};
  if(e->PushLocalFrame(32)!=JNI_OK)return {};std::vector<jvalue>args;const char*p=m.descriptor+1;
  for(const auto&v:a){jvalue j{};switch(*p){case 'Z':j.z=v.number!=0;p++;break;case 'I':j.i=v.number;p++;break;case 'J':j.j=v.number;p++;break;
   case '[':p++;while(*p=='[')p++;if(*p=='L'){while(*p&&*p!=';')p++;if(*p)p++;}else if(*p)p++;j.l=object(e,v);break;
   case 'L':{bool direct=!strncmp(p,"Ljava/nio/ByteBuffer;",20);while(*p&&*p!=';')p++;if(*p)p++;
    if(direct&&v.type==Value::bytes){if(!frameBuffer||frameAddress!=v.bytesData()||frameBytes!=v.bytesSize()){e->DeleteGlobalRef(frameBuffer);frameBuffer=nullptr;auto buffer=object(e,v,true);if(buffer){frameBuffer=e->NewGlobalRef(buffer);e->DeleteLocalRef(buffer);}frameAddress=v.bytesData();frameBytes=v.bytesSize();}j.l=frameBuffer;}else j.l=object(e,v,direct);break;}
   default:e->PopLocalFrame(nullptr);throw std::runtime_error("Browser callback argument type is invalid");}args.push_back(j);}
  Value result;
  if(!e->ExceptionCheck()){const char*returns=strchr(m.descriptor,')')+1;if(*returns=='V')e->CallVoidMethodA(receiver,id,args.data());else if(*returns=='Z')result=Value(bool(e->CallBooleanMethodA(receiver,id,args.data())));else{jobject returned=e->CallObjectMethodA(receiver,id,args.data());if(!e->ExceptionCheck())result=*returns=='['?value(e,static_cast<jbyteArray>(returned)):value(e,static_cast<jstring>(returned));}}
  if(e->ExceptionCheck()){auto error=e->ExceptionOccurred();e->ExceptionClear();if(event!=Event::resource&&report)e->CallStaticVoidMethod(failures,report,error);if(e->ExceptionCheck())e->ExceptionClear();}
  e->PopLocalFrame(nullptr);return result;
 }
};
Value call(JNIEnv*e,Op op,Args a,std::shared_ptr<Callback>c={}){if(e->ExceptionCheck())return {};try{return request(e,op,a,std::move(c));}catch(const std::exception&error){fail(e,error.what());return {};}}
}
}
using namespace klj::web;
#define WEB(name,result,parameters,...) extern "C" JNIEXPORT result JNICALL Java_maclauncher_android_web_WebKitNative_##name parameters {try{__VA_ARGS__}catch(const std::exception&error){fail(e,error.what());return result();}}
WEB(initialize,jstring,(JNIEnv*e,jclass,jstring directory,jstring files,jboolean allowed),return static_cast<jstring>(object(e,call(e,Op::initialize,{value(e,directory),value(e,files),Value(bool(allowed))})));)
WEB(create,jlong,(JNIEnv*e,jclass,jobject callback),return call(e,Op::create,{},std::make_shared<JniCallback>(e,callback)).number;)
WEB(destroy,void,(JNIEnv*e,jclass,jlong h),call(e,Op::destroy,{Value(h)});)
WEB(visibility,void,(JNIEnv*e,jclass,jlong h,jboolean v),call(e,Op::visibility,{Value(h),Value(bool(v))});)
WEB(focus,void,(JNIEnv*e,jclass,jlong h,jboolean v),call(e,Op::focus,{Value(h),Value(bool(v))});)
WEB(size,void,(JNIEnv*e,jclass,jlong h,jint w,jint height,jfloat density),call(e,Op::size,{Value(h),Value(w),Value(height),Value(density)});)
WEB(load,void,(JNIEnv*e,jclass,jlong h,jstring url,jobjectArray keys,jobjectArray values,jbyteArray body),call(e,Op::load,{Value(h),value(e,url),value(e,keys),value(e,values),value(e,body)});)
WEB(html,void,(JNIEnv*e,jclass,jlong h,jstring html,jstring base),call(e,Op::html,{Value(h),value(e,html),value(e,base)});)
WEB(script,void,(JNIEnv*e,jclass,jlong h,jstring script,jlong request),call(e,Op::script,{Value(h),value(e,script),Value(request)});)
WEB(navigate,void,(JNIEnv*e,jclass,jlong h,jint delta),call(e,Op::navigate,{Value(h),Value(delta)});)
WEB(reload,void,(JNIEnv*e,jclass,jlong h),call(e,Op::reload,{Value(h)});)
WEB(stop,void,(JNIEnv*e,jclass,jlong h),call(e,Op::stop,{Value(h)});)
WEB(snapshot,void,(JNIEnv*e,jclass,jlong h),call(e,Op::snapshot,{Value(h)});)
WEB(touch,void,(JNIEnv*e,jclass,jlong h,jint action,jfloat x,jfloat y),call(e,Op::touch,{Value(h),Value(action),Value(x),Value(y)});)
WEB(key,void,(JNIEnv*e,jclass,jlong h,jint action,jint key,jstring text,jint modifiers),call(e,Op::key,{Value(h),Value(action),Value(key),value(e,text),Value(modifiers)});)
WEB(edit,void,(JNIEnv*e,jclass,jlong h,jint operation,jstring text,jint start,jint length),call(e,Op::edit,{Value(h),Value(operation),value(e,text),Value(start),Value(length)});)
WEB(editor,void,(JNIEnv*e,jclass,jlong h,jstring before,jstring after,jstring metadata),call(e,Op::editor,{Value(h),value(e,before),value(e,after),value(e,metadata)});)
WEB(setting,void,(JNIEnv*e,jclass,jlong h,jstring name,jstring json),call(e,Op::setting,{Value(h),value(e,name),value(e,json)});)
WEB(interfaces,void,(JNIEnv*e,jclass,jlong h,jstring json),call(e,Op::interfaces,{Value(h),value(e,json)});)
WEB(dialogResult,void,(JNIEnv*e,jclass,jlong h,jlong request,jboolean accepted,jstring text),call(e,Op::dialogResult,{Value(h),Value(request),Value(bool(accepted)),value(e,text)});)
WEB(state,jbyteArray,(JNIEnv*e,jclass,jlong h),return static_cast<jbyteArray>(object(e,call(e,Op::state,{Value(h)})));)
WEB(restore,jboolean,(JNIEnv*e,jclass,jlong h,jbyteArray state),return call(e,Op::restore,{Value(h),value(e,state)}).number!=0;)
WEB(command,void,(JNIEnv*e,jclass,jlong h,jstring operation,jstring json,jlong request),call(e,Op::command,{Value(h),value(e,operation),value(e,json),Value(request)});)
WEB(cookies,jstring,(JNIEnv*e,jclass,jstring url),return static_cast<jstring>(object(e,call(e,Op::cookies,{value(e,url)})));)
WEB(cookieCommand,void,(JNIEnv*e,jclass,jstring operation,jstring json,jobject callback),call(e,Op::cookieCommand,{value(e,operation),value(e,json)},callback?std::make_shared<JniCallback>(e,callback):nullptr);)
WEB(storageCommand,void,(JNIEnv*e,jclass,jstring operation,jstring json,jobject callback),call(e,Op::storageCommand,{value(e,operation),value(e,json)},callback?std::make_shared<JniCallback>(e,callback):nullptr);)
#undef WEB
