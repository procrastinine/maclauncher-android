#pragma once
#include <jni.h>
#include <stdint.h>
extern "C" jobject klj_surface_composition(JNIEnv*,void*);
extern "C" void *klj_composition_create(JNIEnv*,jobject);
extern "C" void klj_composition_release(void*);
extern "C" int klj_composition_present(JNIEnv*,void*,void*,void*,uint64_t);
