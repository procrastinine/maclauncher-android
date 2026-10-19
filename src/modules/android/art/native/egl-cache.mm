#include "native-egl-cache.hpp"

// The same host-side owner used by JVM EGL. ANGLE calls native callbacks;
// no callback enters Android or another application's storage namespace.
extern "C" void kl_android_egl_cache_install(uintptr_t display,void *(*symbol)(const char*)){
 const char *root=std::getenv("MACLAUNCHER_ANDROID_EGL_CACHE");
 if(!root||!*root||!display||!symbol)return;
 try{
  klj_egl_cache::configure(root,symbol("eglInitialize"));
  klj_egl_cache::install(reinterpret_cast<EGLDisplay>(display),symbol);
 }catch(...){/* Original compilation remains available on a cache miss. */}
}
