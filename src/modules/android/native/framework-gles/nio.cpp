#include <jni.h>
#include <stdint.h>
#include <mutex>
#include <cstring>
#include <nativehelper/JNIPlatformHelp.h>
namespace {
std::once_flag init;
struct Kind{const char* name;const char* array;jint shift;jclass type=nullptr;jfieldID heap=nullptr,offset=nullptr;};
Kind kinds[]={{"java/nio/ByteBuffer","[B",0},{"java/nio/ShortBuffer","[S",1},{"java/nio/CharBuffer","[C",1},{"java/nio/IntBuffer","[I",2},{"java/nio/FloatBuffer","[F",2},{"java/nio/LongBuffer","[J",3},{"java/nio/DoubleBuffer","[D",3}};
jfieldID position,limit;
void setup(JNIEnv*e){jclass base=e->FindClass("java/nio/Buffer");position=e->GetFieldID(base,"position","I");limit=e->GetFieldID(base,"limit","I");e->DeleteLocalRef(base);for(auto&k:kinds){jclass t=e->FindClass(k.name);k.type=(jclass)e->NewGlobalRef(t);e->DeleteLocalRef(t);k.heap=e->GetFieldID(k.type,"hb",k.array);k.offset=e->GetFieldID(k.type,"offset","I");}}
Kind* kind(JNIEnv*e,jobject buffer){std::call_once(init,setup,e);if(!buffer){jniThrowException(e,"java/lang/IllegalArgumentException","buffer is null");return nullptr;}for(auto&k:kinds)if(e->IsInstanceOf(buffer,k.type))return &k;jniThrowException(e,"java/lang/IllegalArgumentException","Unknown NIO buffer storage");return nullptr;}
}
int jniThrowException(JNIEnv*e,const char*name,const char*message){if(e->ExceptionCheck())return -1;jclass type=e->FindClass(name);if(!type)return -1;int result=e->ThrowNew(type,message);e->DeleteLocalRef(type);return result;}
jlong jniGetNioBufferFields(JNIEnv*e,jobject buffer,jint*p,jint*l,jint*s){Kind*k=kind(e,buffer);if(!k)return 0;*p=e->GetIntField(buffer,position);*l=e->GetIntField(buffer,limit);*s=k->shift;return (jlong)(uintptr_t)e->GetDirectBufferAddress(buffer);}
jarray jniGetNioBufferBaseArray(JNIEnv*e,jobject buffer){Kind*k=kind(e,buffer);return k?(jarray)e->GetObjectField(buffer,k->heap):nullptr;}
jint jniGetNioBufferBaseArrayOffset(JNIEnv*e,jobject buffer){Kind*k=kind(e,buffer);return k?((e->GetIntField(buffer,k->offset)+e->GetIntField(buffer,position))<<k->shift):0;}
