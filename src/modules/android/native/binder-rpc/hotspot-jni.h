#pragma once
#include <jni.h>
JNIEnv* kljBinderEnvironment(JavaVM*);

// ART's critical entrypoints omit the two JNI context arguments. Derive the
// normal JVM entrypoint from each supplied function's actual C++ signature.
template <auto Function> struct KlJni;
template <typename R, typename... Args, R (*Function)(Args...)>
struct KlJni<Function> {
    static R call(JNIEnv*, jclass, Args... args) { return Function(args...); }
};
template <typename R,typename Owner,typename... Args,R (*Function)(JNIEnv*,Owner,Args...)>
struct KlJni<Function> {static constexpr auto call=Function;};

extern "C" void __android_log_assert(const char*, const char*, const char*, ...);
extern "C" int __android_log_write(int, const char*, const char*);
extern "C" int __android_log_print(int, const char*, const char*, ...);

// Native Parcel retires descriptors through the existing description owner,
// including any readonly handle and exact-file capability lease.
extern "C" int klj_parcel_close(int descriptor);
