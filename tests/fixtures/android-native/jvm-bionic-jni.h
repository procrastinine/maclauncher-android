#pragma once
#include <jni.h>
#include <stdint.h>
typedef struct {uint64_t function;JNIEnv *env;jclass type;jvalue values[10];jlong result;} FixtureJniCall;
typedef struct {uint64_t dispatcher,typed_dispatcher;const char *library;} FixtureJvm;
typedef struct {JNIEnv *env;jclass type;jmethodID method;jvalue argument;jlong result;} FixtureJavaCallback;
