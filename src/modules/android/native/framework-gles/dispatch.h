#pragma once
#include <jni.h>
#include <atomic>
#include <type_traits>
#include <climits>
#include <cstdint>
#define GL_GLES_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
using uint=unsigned int;
void* klj_framework_graphics_symbol(const char*);
void klj_framework_graphics_missing(const char*);
template<class T,const char*Name> struct HostGraphicsEntry;
template<class R,class...A,const char*Name> struct HostGraphicsEntry<R(*)(A...),Name> {
 using Fn=R(*)(A...);
 static R call(A...args){static std::atomic<Fn> address{nullptr};Fn fn=address.load(std::memory_order_relaxed);if(!fn){fn=reinterpret_cast<Fn>(klj_framework_graphics_symbol(Name));if(!fn){klj_framework_graphics_missing(Name);if constexpr(!std::is_void_v<R>)return R{};else return;}address.store(fn,std::memory_order_relaxed);}return fn(args...);}
};
