#include "xr-surface.h"
#include "host-callbacks.h"
#include "jni-invocation.h"
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>

/* This surface belongs to the same actual VM as the ordinary application.
 * Native renderer threads use the existing guest-thread attachment owner. */
static _Atomic(KLHostCallbacks *) host_callbacks;
extern void klj_enter_guest_thread(void);
static JNIEnv *environment(void){
    JavaVM *vm=klj_jni_actual_vm();
    JNIEnv *env=NULL;if(!vm)return NULL;
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_OK)return env;
    if(state!=JNI_EDETACHED)return NULL;
    klj_enter_guest_thread();
    return (*vm)->AttachCurrentThreadAsDaemon(vm,(void**)&env,NULL)==JNI_OK?env:NULL;
}
static KLHostCallbacks *callbacks(JNIEnv *env){
    KLHostCallbacks *value=atomic_load_explicit(&host_callbacks,memory_order_acquire);
    if(value)return value;
    KLHostCallbacks *made=kl_host_callbacks_create(env);if(!made)return NULL;
    if(!atomic_compare_exchange_strong_explicit(&host_callbacks,&value,made,memory_order_release,memory_order_acquire)){
        kl_host_callbacks_release(env,made);return value;
    }
    return made;
}
void klj_xr_surface_shutdown(JNIEnv *env){
    kl_host_callbacks_release(env,atomic_exchange_explicit(&host_callbacks,NULL,memory_order_acq_rel));
}

struct KljXrSurface {jobject owner,surface;uint64_t identity;};

void klj_xr_surface_release(KljXrSurface *surface){
    if(!surface)return;
    JNIEnv *env=environment();
    if(env){
        jthrowable prior=(*env)->ExceptionOccurred(env);if(prior)(*env)->ExceptionClear(env);
        KLHostCallbacks *bindings=callbacks(env);
        if(surface->owner&&bindings&&klj_jni_before_java(env)){
            kl_host_xr_surface_release(env,bindings,kl_host_bind_KLHost_maclauncher_android_framework_XrSurface(env,bindings,surface->owner));klj_jni_after_java(env);
        }
        if(surface->owner)(*env)->DeleteGlobalRef(env,surface->owner);
        if(surface->surface)(*env)->DeleteGlobalRef(env,surface->surface);
        if(prior){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,prior);(*env)->DeleteLocalRef(env,prior);}
    }
    free(surface);
}
KljXrSurface *klj_xr_surface_create(uint32_t width,uint32_t height){
    JNIEnv *env=environment();
    if(!env||!width||!height||width>INT_MAX||height>INT_MAX||(*env)->ExceptionCheck(env))return NULL;
    KLHostCallbacks *bindings=callbacks(env);
    if(!bindings)return NULL;
    KljXrSurface *value=calloc(1,sizeof *value);if(!value)return NULL;
    if(!klj_jni_before_java(env)){free(value);return NULL;}
    KLHost_maclauncher_android_framework_XrSurface owner=kl_host_xr_surface_create(env,bindings,(jint)width,(jint)height);
    if(owner.object){
        value->owner=(*env)->NewGlobalRef(env,owner.object);
        jobject surface=(*env)->ExceptionCheck(env)?NULL:kl_host_xr_surface_object(env,bindings,owner);
        if(surface){value->surface=(*env)->NewGlobalRef(env,surface);(*env)->DeleteLocalRef(env,surface);}
        if(!(*env)->ExceptionCheck(env))value->identity=(uint64_t)kl_host_xr_surface_identity(env,bindings,owner);
        if(!value->owner){
            jthrowable failure=(*env)->ExceptionOccurred(env);if(failure)(*env)->ExceptionClear(env);
            kl_host_xr_surface_release(env,bindings,owner);
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
