#pragma once
#include <jni.h>
#include <array>
#include <cstdint>
#include <nativehelper/JNIPlatformHelp.h>
/** HotSpot releases require the allocation base, whereas generated Android JNI
 * sometimes retains only the positioned pointer. Retain actual JNI ownership. */
namespace HostArrayAccess {
struct Lease {void*base;uintptr_t end;};
inline thread_local std::array<Lease,64> active{};
inline thread_local size_t count=0;
inline bool reserve(JNIEnv*e){if(count<active.size())return true;jniThrowException(e,"java/lang/IllegalStateException","Too many simultaneous graphics array leases");return false;}
inline void remember(void*p,size_t bytes){if(p)active[count++]={p,(uintptr_t)p+bytes};}
inline void*base(JNIEnv*e,void*p){uintptr_t at=(uintptr_t)p;for(size_t i=count;i;i--){const auto&lease=active[i-1];if(at>=(uintptr_t)lease.base&&at<=lease.end){void*result=lease.base;for(size_t j=i;j<count;j++)active[j-1]=active[j];--count;return result;}}jniThrowException(e,"java/lang/IllegalArgumentException","Graphics array storage is not owned by this call");return nullptr;}
#define HOST_ARRAY(NAME,TYPE,ARRAY) \
inline TYPE*get##NAME(JNIEnv*e,ARRAY array,jboolean*copy){if(!reserve(e))return nullptr;auto length=e->GetArrayLength(array);if(e->ExceptionCheck())return nullptr;TYPE*p=e->Get##NAME##ArrayElements(array,copy);remember(p,(size_t)length*sizeof(TYPE));return p;} \
inline void release##NAME(JNIEnv*e,ARRAY array,TYPE*p,jint mode){void*origin=base(e,p);if(origin)e->Release##NAME##ArrayElements(array,(TYPE*)origin,mode);}
HOST_ARRAY(Byte,jbyte,jbyteArray) HOST_ARRAY(Boolean,jboolean,jbooleanArray) HOST_ARRAY(Char,jchar,jcharArray) HOST_ARRAY(Short,jshort,jshortArray) HOST_ARRAY(Int,jint,jintArray) HOST_ARRAY(Long,jlong,jlongArray) HOST_ARRAY(Float,jfloat,jfloatArray) HOST_ARRAY(Double,jdouble,jdoubleArray)
#undef HOST_ARRAY
}
