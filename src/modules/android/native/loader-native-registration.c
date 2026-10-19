#include <jni.h>
#include <jvmti.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include "jni-lifetime.h"
extern jvmtiEnv *klj_jni_vm_tooling(JNIEnv *);
extern void *klj_native_abi(void*,const char*,char*,size_t);
static pthread_mutex_t registration_mutex=PTHREAD_MUTEX_INITIALIZER;
static jint (JNICALL *previous_register)(JNIEnv*,jclass,const JNINativeMethod*,jint);
static int guest_hooks_installed;
typedef struct GuestRange {uintptr_t base,end;struct GuestRange *next;} GuestRange;
static _Atomic(GuestRange*) ranges;
static int add_range(uintptr_t start,size_t length){
 GuestRange *head=atomic_load_explicit(&ranges,memory_order_acquire);
 for(GuestRange *range=head;range;range=range->next)if(range->base==start&&range->end==start+length)return 1;
 GuestRange *range=malloc(sizeof(*range));if(!range)return 0;
 *range=(GuestRange){start,start+length,head};atomic_store_explicit(&ranges,range,memory_order_release);return 1;
}
typedef struct {const char *(*name)(const void*,size_t*);void *(*find)(const char*);void *(*base)(void*);size_t(*span)(void*);} Registry;
static Registry registry_storage;static _Atomic(Registry*) registry;
int klj_guest_address(const void *address){
 uintptr_t pointer=(uintptr_t)address;
 for(GuestRange *range=atomic_load_explicit(&ranges,memory_order_acquire);range;range=range->next)if(pointer>=range->base&&pointer<range->end)return 1;
 Registry *source=atomic_load_explicit(&registry,memory_order_acquire);if(!pointer||!source)return 0;
 // Klepton's append-only registry also contains libraries loaded by guest dlopen.
 // Positive ranges are cached; allocation failure still validates genuine images.
 size_t offset=0;const char *name=source->name(address,&offset);if(!name)return 0;
 void *image=source->find(name);if(!image)return 0;
 uintptr_t start=(uintptr_t)source->base(image);size_t length=source->span(image);
 if(!start||!length||length>UINTPTR_MAX-start||pointer<start||pointer>=start+length||offset!=pointer-start)return 0;
 pthread_mutex_lock(&registration_mutex);add_range(start,length);pthread_mutex_unlock(&registration_mutex);return 1;
}
#define guest_address klj_guest_address
static void registration_failure(JNIEnv *env,const char *message){if((*env)->ExceptionCheck(env))return;jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");if(type)(*env)->ThrowNew(env,type,message);}
extern int klj_framework_registration(JNIEnv*,jclass,const JNINativeMethod*,jint,JNINativeMethod**,jint*);
static jint JNICALL register_guest(JNIEnv *env,jclass target,const JNINativeMethod *methods,jint count){
    if((*env)->ExceptionCheck(env))return JNI_ERR;
    if(count<0||(count&&!methods)||(size_t)count>SIZE_MAX/sizeof(*methods)){registration_failure(env,"Invalid native method table");return JNI_ERR;}
    JNINativeMethod *adapted=NULL;
    if(klj_framework_registration(env,target,methods,count,&adapted,&count)<0)return JNI_ERR;
    if(adapted)methods=adapted;
    int changed=0;for(jint i=0;i<count;i++){if(methods[i].fnPtr&&guest_address(methods[i].fnPtr)){changed=1;break;}}
    if(!changed){jint result=count?previous_register(env,target,methods,count):JNI_OK;free(adapted);return result;}
    if(!adapted)adapted=malloc((size_t)count*sizeof(*adapted));
    if(!adapted){registration_failure(env,"Native method table allocation failed");return JNI_ERR;}
    if(adapted!=methods)memcpy(adapted,methods,(size_t)count*sizeof(*methods));
    for(jint i=0;i<count;i++){
        if(!adapted[i].fnPtr||!guest_address(adapted[i].fnPtr))continue;
        if(!adapted[i].signature){free(adapted);registration_failure(env,"Native method descriptor is missing");return JNI_ERR;}
        char error[512];adapted[i].fnPtr=klj_native_abi(adapted[i].fnPtr,adapted[i].signature,error,sizeof error);
        if(!adapted[i].fnPtr){free(adapted);registration_failure(env,error);return JNI_ERR;}
    }
    jint result=previous_register(env,target,adapted,count);free(adapted);return result;
}
/* One owner installs the RegisterNatives dispatcher. Framework registration
 * precedes guest loading; later ABI/lookup hooks extend the existing table. */
int klj_registration_install(JNIEnv *env){
    pthread_mutex_lock(&registration_mutex);
    if(previous_register){pthread_mutex_unlock(&registration_mutex);return 1;}
    jvmtiEnv *ti=klj_jni_vm_tooling(env);jniNativeInterface *table=NULL;
    int okay=ti!=NULL;
    if(okay)okay=(*ti)->GetJNIFunctionTable(ti,&table)==JVMTI_ERROR_NONE;
    if(okay){extern int klj_field_lookup_prepare(JNIEnv*,jniNativeInterface*);okay=klj_field_lookup_prepare(env,table);}
    if(okay){previous_register=table->RegisterNatives;table->RegisterNatives=register_guest;okay=(*ti)->SetJNIFunctionTable(ti,table)==JVMTI_ERROR_NONE;if(!okay)previous_register=NULL;}
    if(table)(*ti)->Deallocate(ti,(unsigned char*)table);
    pthread_mutex_unlock(&registration_mutex);
    if(!okay)registration_failure(env,"Android native registration is unavailable");return okay;
}
int klj_registration_prepare(JNIEnv *env,void *runtime,void *image){
    if((*env)->ExceptionCheck(env))return 0;
    if(!klj_registration_install(env))return 0;
    pthread_mutex_lock(&registration_mutex);
    void *(*base)(void*)=dlsym(runtime,"kl_base");size_t(*span)(void*)=dlsym(runtime,"kl_span");
    if(!atomic_load_explicit(&registry,memory_order_relaxed)){
        Registry resolved={dlsym(runtime,"kl_addr_image"),dlsym(runtime,"kl_find_image"),base,span};
        if(resolved.name&&resolved.find&&resolved.base&&resolved.span){registry_storage=resolved;atomic_store_explicit(&registry,&registry_storage,memory_order_release);}
    }
    uintptr_t start=base?(uintptr_t)base(image):0;size_t length=span?span(image):0;
    if(!start||!length||length>UINTPTR_MAX-start){pthread_mutex_unlock(&registration_mutex);registration_failure(env,"Invalid native image registration range");return 0;}
    if(guest_hooks_installed){int tracked=add_range(start,length)||atomic_load_explicit(&registry,memory_order_acquire)!=NULL;pthread_mutex_unlock(&registration_mutex);if(!tracked)registration_failure(env,"Native image range allocation failed");return tracked;}
    jvmtiEnv *ti=klj_jni_vm_tooling(env);jniNativeInterface *table=NULL;
    int okay=ti!=NULL;
    if(okay)okay=(*ti)->GetJNIFunctionTable(ti,&table)==JVMTI_ERROR_NONE;
    if(okay){extern int klj_jni_calls_prepare(JNIEnv*,jniNativeInterface*,jvmtiEnv*);extern int klj_class_lookup_prepare(JNIEnv*,jniNativeInterface*);okay=klj_class_lookup_prepare(env,table)&&klj_jni_calls_prepare(env,table,ti)&&(*ti)->SetJNIFunctionTable(ti,table)==JVMTI_ERROR_NONE;if(okay)guest_hooks_installed=1;}
    if(table)(*ti)->Deallocate(ti,(unsigned char*)table);
    // The shared VM owner also retains lifecycle callbacks before guest hooks.
    // Failed hook setup cannot dispose that process-wide environment.
    if(!okay&&ti){
        extern void klj_jni_calls_abort(jvmtiEnv*);klj_jni_calls_abort(ti);
    }
    // A failed hook setup leaves the image unpublished and may unmap it. Do not
    // retain its address range as guest code after that allocation is released.
    if(okay)okay=add_range(start,length)||atomic_load_explicit(&registry,memory_order_acquire)!=NULL;
    pthread_mutex_unlock(&registration_mutex);
    if(!okay)registration_failure(env,"Android dynamic native registration is unavailable");
    return okay;
}
