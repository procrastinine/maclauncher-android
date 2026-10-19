#include "hotspot-jni.h"
#include "android_util_CharsetUtils.cpp"
#include <memory>
#include <new>

namespace android {
static jint encodeArray(JNIEnv* env,jclass owner,jstring value,jint length,jbyteArray array,jint offset,jint capacity){
    ScopedByteArrayRW bytes(env,array);
    if(!bytes.get())return 0;
    if(capacity<0||capacity>bytes.size()){
        jniThrowException(env,"java/lang/ArrayIndexOutOfBoundsException","Modified UTF destination exceeds its array");return 0;
    }
    return android_util_CharsetUtils_toModifiedUtf8Bytes(env,owner,value,length,
        reinterpret_cast<jlong>(bytes.get()),offset,capacity);
}
static jstring decodeArray(JNIEnv* env,jclass owner,jbyteArray array,jint offset,jint length){
    ScopedByteArrayRW bytes(env,array);
    if(!bytes.get())return nullptr;
    if(offset<0||length<0||offset>bytes.size()||length>bytes.size()-offset){
        jniThrowException(env,"java/lang/ArrayIndexOutOfBoundsException","Modified UTF source exceeds its array");return nullptr;
    }
    // ART arrays have allocation padding beyond their logical end. HotSpot's
    // scoped lease promises only the declared elements, so provide the codec's
    // temporary terminator when a valid buffered string reaches that end.
    if(length==bytes.size()-offset){
        std::unique_ptr<jbyte[]> padded(new(std::nothrow) jbyte[static_cast<size_t>(length)+1]);
        if(!padded){jniThrowException(env,"java/lang/OutOfMemoryError","Modified UTF temporary buffer");return nullptr;}
        memcpy(padded.get(),bytes.get()+offset,length);padded[length]=0;
        return android_util_CharsetUtils_fromModifiedUtf8Bytes(env,owner,
            reinterpret_cast<jlong>(padded.get()),0,length);
    }
    return android_util_CharsetUtils_fromModifiedUtf8Bytes(env,owner,
        reinterpret_cast<jlong>(bytes.get()),offset,length);
}
int register_android_util_CharsetArray(JNIEnv* env){
    static const JNINativeMethod methods[]={
        {"encode","(Ljava/lang/String;I[BII)I",reinterpret_cast<void*>(encodeArray)},
        {"fromModifiedUtf8Bytes","([BII)Ljava/lang/String;",reinterpret_cast<void*>(decodeArray)}
    };
    return RegisterMethodsOrDie(env,"maclauncher/android/framework/HostCharset",methods,NELEM(methods));
}
}
