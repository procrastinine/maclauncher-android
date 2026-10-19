#pragma once
#include <jni.h>
#include <stdint.h>
extern "C" bool klj_surface_texture_owner(uint64_t lease);
extern "C" bool klj_surface_is_texture(void *window);
extern "C" uint64_t klj_surface_lease(void *window);
extern "C" void *klj_surface_copy_storage(void *window);
extern "C" unsigned klj_texture_publish(JNIEnv*,void *display,void *window);
