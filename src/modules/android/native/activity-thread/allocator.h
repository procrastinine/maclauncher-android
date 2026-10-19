#pragma once
#include <jni.h>
#include <malloc/malloc.h>
#include <errno.h>
#include <stddef.h>

// AOSP's idle purge is an allocator operation. Reclaim unused pages from every
// registered Darwin zone without releasing live Android/native resource owners.
static int klj_activity_mallopt(int operation,int value){
    (void)value;
    if(operation!=M_PURGE){errno=ENOTSUP;return 0;}
    malloc_zone_pressure_relief(nullptr,0);
    return 1;
}

static bool klj_activity_android_mallopt(int operation,void* argument,size_t size){
    (void)operation;(void)argument;(void)size;
    // This JVM is not an Android zygote child and Darwin has no Bionic heap
    // profiling backend to initialize. Preserve the allocator's failure result;
    // the original optional ActivityThread caller deliberately ignores it.
    errno=ENOTSUP;
    return false;
}

static int klj_activity_register(JNIEnv* env,const char* name,const JNINativeMethod* methods,int count){
    jclass type=env->FindClass(name);if(!type)return JNI_ERR;
    int result=env->RegisterNatives(type,methods,count);env->DeleteLocalRef(type);return result;
}
#define mallopt klj_activity_mallopt
#define android_mallopt klj_activity_android_mallopt
#define RegisterMethodsOrDie klj_activity_register
#define NELEM(array) (sizeof(array)/sizeof((array)[0]))
