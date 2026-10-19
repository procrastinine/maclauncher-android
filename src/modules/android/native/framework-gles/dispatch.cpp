#include "dispatch.h"
#include <nativehelper/JNIPlatformHelp.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <string>
extern "C" void* klj_native_egl_lookup(const char*);
extern "C" void* klj_graphics_symbol(const char*);
extern "C" void* klj_surface_native_window(JNIEnv*,jobject);
extern "C" void klj_surface_release_window(void*);
extern "C" int klj_native_egl_init_count(void*);
extern "C" void klj_native_egl_error(unsigned);
namespace {thread_local unsigned glFailure;std::mutex diagnostics;std::unordered_set<std::string> reported;unsigned glError(){unsigned result=glFailure;glFailure=0;if(result)return result;auto call=(unsigned(*)())klj_graphics_symbol("glGetError");return call?call():0x0502;}}
void*klj_framework_graphics_symbol(const char*name){if(!strcmp(name,"glGetError"))return (void*)glError;return klj_native_egl_lookup(name);}
void klj_framework_graphics_missing(const char*name){if(!strncmp(name,"gl",2))glFailure=0x0502;else klj_native_egl_error(0x300B);std::lock_guard lock(diagnostics);if(reported.size()<64&&reported.emplace(name).second)std::fprintf(stderr,"[android-graphics] Native operation is unavailable: %s\n",name);}
extern "C" void*klj_framework_acquire_window(JNIEnv*e,jobject value){return klj_surface_native_window(e,value);}
extern "C" void klj_framework_release_window(void*value){klj_surface_release_window(value);}
namespace android {int egl_get_init_count(void*display){return klj_native_egl_init_count(display);}}
// Android's generated bindings pass the Java buffer extent to these platform
// entry points. ANGLE owns the actual vertex-array state and pointer lifetime.
#define BOUNDS3(NAME,T) extern "C" void NAME##Bounds(GLint size,GLenum type,GLsizei stride,const GLvoid*pointer,GLsizei){using F=void(*)(GLint,GLenum,GLsizei,const GLvoid*);static auto fn=(F)klj_framework_graphics_symbol(#NAME);if(fn)fn(size,type,stride,pointer);else klj_framework_graphics_missing(#NAME);}
BOUNDS3(glColorPointer,int) BOUNDS3(glTexCoordPointer,int) BOUNDS3(glVertexPointer,int) BOUNDS3(glMatrixIndexPointerOES,int) BOUNDS3(glWeightPointerOES,int)
extern "C" void glNormalPointerBounds(GLenum type,GLsizei stride,const GLvoid*pointer,GLsizei){using F=void(*)(GLenum,GLsizei,const GLvoid*);static auto fn=(F)klj_framework_graphics_symbol("glNormalPointer");if(fn)fn(type,stride,pointer);else klj_framework_graphics_missing("glNormalPointer");}
extern "C" void glPointSizePointerOESBounds(GLenum type,GLsizei stride,const GLvoid*pointer,GLsizei){using F=void(*)(GLenum,GLsizei,const GLvoid*);static auto fn=(F)klj_framework_graphics_symbol("glPointSizePointerOES");if(fn)fn(type,stride,pointer);else klj_framework_graphics_missing("glPointSizePointerOES");}
