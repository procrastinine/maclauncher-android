#include "xr-surface.h"
#include "device-context.h"
#include "jni-invocation.h"
#include <stdlib.h>
#include <limits.h>

struct KljXrSurface {jobject owner,surface;uint64_t identity;};

void klj_xr_surface_release(KljXrSurface *surface){
    if(!surface)return;
    JNIEnv *env=klj_device_environment();
    if(env){
        jthrowable prior=(*env)->ExceptionOccurred(env);if(prior)(*env)->ExceptionClear(env);
        KLHostCallbacks *callbacks=klj_device_callbacks();
        if(surface->owner&&callbacks&&klj_jni_before_java(env)){
            kl_host_xr_surface_release(env,callbacks,kl_host_bind_KLHost_maclauncher_android_framework_XrSurface(env,callbacks,surface->owner));klj_jni_after_java(env);
        }
        if(surface->owner)(*env)->DeleteGlobalRef(env,surface->owner);
        if(surface->surface)(*env)->DeleteGlobalRef(env,surface->surface);
        if(prior){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,prior);(*env)->DeleteLocalRef(env,prior);}
    }
    free(surface);
}
KljXrSurface *klj_xr_surface_create(uint32_t width,uint32_t height){
    JNIEnv *env=klj_device_environment();
    if(!env||!width||!height||width>INT_MAX||height>INT_MAX||(*env)->ExceptionCheck(env))return NULL;
    KLHostCallbacks *callbacks=klj_device_callbacks();
    if(!callbacks)return NULL;
    KljXrSurface *value=calloc(1,sizeof *value);if(!value)return NULL;
    if(!klj_jni_before_java(env)){free(value);return NULL;}
    KLHost_maclauncher_android_framework_XrSurface owner=kl_host_xr_surface_create(env,callbacks,(jint)width,(jint)height);
    if(owner.object){
        value->owner=(*env)->NewGlobalRef(env,owner.object);
        jobject surface=(*env)->ExceptionCheck(env)?NULL:kl_host_xr_surface_object(env,callbacks,owner);
        if(surface){value->surface=(*env)->NewGlobalRef(env,surface);(*env)->DeleteLocalRef(env,surface);}
        if(!(*env)->ExceptionCheck(env))value->identity=(uint64_t)kl_host_xr_surface_identity(env,callbacks,owner);
        if(!value->owner){
            jthrowable failure=(*env)->ExceptionOccurred(env);if(failure)(*env)->ExceptionClear(env);
            kl_host_xr_surface_release(env,callbacks,owner);
            if(failure){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,failure);(*env)->DeleteLocalRef(env,failure);}
        }
        (*env)->DeleteLocalRef(env,owner.object);
    }
    klj_jni_after_java(env);
    if(!value->owner||!value->surface||!value->identity||(*env)->ExceptionCheck(env)){klj_xr_surface_release(value);return NULL;}
    return value;
}
void *klj_xr_surface_object(KljXrSurface *surface){return surface?surface->surface:NULL;}
int klj_xr_surface_matches(KljXrSurface *surface,const void *native_window){
    return surface&&surface->identity==(uint64_t)(uintptr_t)native_window;
}
