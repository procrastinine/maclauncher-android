#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "presentation.h"

static pthread_once_t symbols_once=PTHREAD_ONCE_INIT;
static void *egl_library,*gles_library;
static char load_error[1024];
// These libraries remain loaded for the process lifetime. Cache symbol identity
// with the same lifetime, including absent optional exports. Render threads only
// read immutable entries; the mutex is used once when a new name is encountered.
typedef struct graphics_symbol_entry {
    struct graphics_symbol_entry *next;
    void *address;
    char name[];
} graphics_symbol_entry;
static _Atomic(graphics_symbol_entry*) symbol_cache[256];
static pthread_mutex_t symbol_cache_lock=PTHREAD_MUTEX_INITIALIZER;
static void load_symbols(void){
    const char *root=getenv("KL_ANGLE_DIR");char filename[4096];
    if(!root||!*root){snprintf(load_error,sizeof load_error,"The Android ANGLE library directory is missing");return;}
    if(snprintf(filename,sizeof filename,"%s/libEGL.dylib",root)>=(int)sizeof filename)return;
    // ANGLE may depend on desktop OpenGL. A provider handle must resolve only
    // its own exports: dependency symbols require another context/API owner.
    // Extension-only entries still use this provider's eglGetProcAddress below.
    egl_library=dlopen(filename,RTLD_NOW|RTLD_LOCAL|RTLD_FIRST);
    if(!egl_library){snprintf(load_error,sizeof load_error,"%s",dlerror());return;}
    if(snprintf(filename,sizeof filename,"%s/libGLESv2.dylib",root)>=(int)sizeof filename)return;
    gles_library=dlopen(filename,RTLD_NOW|RTLD_LOCAL|RTLD_FIRST);
    if(!gles_library)snprintf(load_error,sizeof load_error,"%s",dlerror());
}
// JVM-native calls already use Darwin's ABI; no Android stack-argument thunk.
// Loading symbols itself does not initialize an EGL display or GPU context.
void *klj_graphics_symbol(const char *name){
    if(!name)return NULL;
    pthread_once(&symbols_once,load_symbols);
    unsigned hash=2166136261u;for(const unsigned char *at=(const unsigned char*)name;*at;at++)hash=(hash^*at)*16777619u;
    unsigned bucket=hash%256;
    for(graphics_symbol_entry *entry=atomic_load_explicit(&symbol_cache[bucket],memory_order_acquire);entry;entry=entry->next)if(!strcmp(entry->name,name))return entry->address;
    pthread_mutex_lock(&symbol_cache_lock);
    graphics_symbol_entry *head=atomic_load_explicit(&symbol_cache[bucket],memory_order_acquire);
    for(graphics_symbol_entry *entry=head;entry;entry=entry->next)if(!strcmp(entry->name,name)){pthread_mutex_unlock(&symbol_cache_lock);return entry->address;}
    // An EGL lookup in libGLES first walks its entire dependency closure before
    // failing. Query the owning library first for both namespaces.
    int is_egl=!strncmp(name,"egl",3);
    void *first=is_egl?egl_library:gles_library,*second=is_egl?gles_library:egl_library;
    void *p=first?dlsym(first,name):NULL;
    if(!p&&second)p=dlsym(second,name);
    if(!p&&egl_library){void *(*get)(const char*)=dlsym(egl_library,"eglGetProcAddress");if(get)p=get(name);}
    graphics_symbol_entry *entry=malloc(sizeof(*entry)+strlen(name)+1);
    if(entry){entry->next=head;entry->address=p;strcpy(entry->name,name);atomic_store_explicit(&symbol_cache[bucket],entry,memory_order_release);}
    pthread_mutex_unlock(&symbol_cache_lock);
    return p;
}

JNIEXPORT void JNICALL Java_maclauncher_android_bridge_Graphics_configureRuntime(JNIEnv *env,jclass ignored,jstring angleDirectory){
    (void)ignored;
    const char *angle=(*env)->GetStringUTFChars(env,angleDirectory,NULL);if(!angle)return;
    setenv("KL_ANGLE_DIR",angle,1);(*env)->ReleaseStringUTFChars(env,angleDirectory,angle);
}
extern unsigned klj_vulkan_api_version(void);
extern int klj_vulkan_api_level(void);
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_Graphics_vulkanVersion(JNIEnv *env,jclass ignored){
    (void)env;(void)ignored;
    // Managed selection is inherited before this process starts. Probe only
    // that driver; a GLES-only process does not search unrelated host files.
    const char *selected=getenv("KL_MVK_DYLIB");return selected&&*selected?(jint)klj_vulkan_api_version():0;
}
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_Graphics_vulkanHardwareLevel(JNIEnv *env,jclass ignored){
    (void)env;(void)ignored;
    const char *selected=getenv("KL_MVK_DYLIB");return selected&&*selected?(jint)klj_vulkan_api_level():-1;
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_Graphics_configureHost(JNIEnv *env,jclass ignored,jstring library,jstring angleDirectory,jint width,jint height){
    Java_maclauncher_android_bridge_Graphics_configureRuntime(env,ignored,angleDirectory);
    if((*env)->ExceptionCheck(env))return;
    char dimensions[40];snprintf(dimensions,sizeof dimensions,"%dx%d",width,height);
    setenv("KL_GLFB_SIZE",dimensions,1);setenv("KL_GLFB","1",1);unsetenv("KL_PERMISSIVE");
    setenv("KLEPTON_METAL_PRESENT","1",1);
    setenv("RESHADE_EXPLICIT_HOST","1",1);
    if(library){const char *path=(*env)->GetStringUTFChars(env,library,NULL);if(!path)return;setenv("KLEPTON_FRAME_LIBRARY",path,1);(*env)->ReleaseStringUTFChars(env,library,path);}
    else unsetenv("KLEPTON_FRAME_LIBRARY");
}
