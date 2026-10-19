#pragma once
#include <jni.h>
#include <stdint.h>
jlong jniGetNioBufferFields(JNIEnv*,jobject,jint*,jint*,jint*);
jarray jniGetNioBufferBaseArray(JNIEnv*,jobject);
jint jniGetNioBufferBaseArrayOffset(JNIEnv*,jobject);
int jniThrowException(JNIEnv*,const char*,const char*);
