#pragma once
#include <stdint.h>
#include <stddef.h>
#include <jni.h>
#include "thread-admission.h"
typedef struct { uint64_t constructor_tid,constructor_pthread,constructor_tp,errno_address;unsigned constructors,key_destructors,cxa_destructors,calls,nested,callbacks,ended; } State;
typedef struct { uint64_t prepare,join,dispatch,nested,state; } Bootstrap;
typedef struct { uint64_t token,thread; int error; } Prepared;
typedef struct { uint64_t function;JNIEnv *env;jclass type;jvalue values[10];jlong result; } Typed;
typedef struct { JNIEnv *env;jclass type;jlong value,result; } JavaCallback;
int64_t fixture_host(uint32_t,void*,size_t);
State *fixture_state(void);
