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
/* Optional real-image provider; an absent provider leaves legacy routing intact. */
extern int klj_jni_provider_address(const void *) __attribute__((weak_import));
extern void *klj_jni_provider_native_abi(void *,const char *,char *,size_t) __attribute__((weak_import));
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
 if(klj_jni_provider_address&&klj_jni_provider_address(address))return 1;
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
        char error[512];
        if(klj_jni_provider_address&&klj_jni_provider_address(adapted[i].fnPtr)){
            adapted[i].fnPtr=klj_jni_provider_native_abi?klj_jni_provider_native_abi(adapted[i].fnPtr,adapted[i].signature,error,sizeof error):NULL;
            if(!klj_jni_provider_native_abi)snprintf(error,sizeof error,"Native image ABI owner is unavailable");
        }else adapted[i].fnPtr=klj_native_abi(adapted[i].fnPtr,adapted[i].signature,error,sizeof error);
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
static int prepare_hooks_locked(JNIEnv *env){
    if(guest_hooks_installed)return 1;
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
    return okay;
}
int klj_registration_prepare_facade(JNIEnv *env){
    if((*env)->ExceptionCheck(env)||!klj_registration_install(env))return 0;
    pthread_mutex_lock(&registration_mutex);int okay=prepare_hooks_locked(env);pthread_mutex_unlock(&registration_mutex);
    if(!okay)registration_failure(env,"Android JNI facade is unavailable");return okay;
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
    int okay=prepare_hooks_locked(env);
    // A failed hook setup leaves the image unpublished and may unmap it. Do not
    // retain its address range as guest code after that allocation is released.
    if(okay)okay=add_range(start,length)||atomic_load_explicit(&registry,memory_order_acquire)!=NULL;
    pthread_mutex_unlock(&registration_mutex);
    if(!okay)registration_failure(env,"Android dynamic native registration is unavailable");
    return okay;
}

/* Explicit exported-method staging is separate from register_guest. Guest and
 * JDK RegisterNatives/UnregisterNatives retain their immediate original routes. */
#include "jni-registration.h"
typedef struct ExportRegistration {
 jclass declaring;jmethodID method;JNINativeMethod native;
 struct ExportRegistration *next;
} ExportRegistration;
enum { EXPORTS_STAGING=1,EXPORTS_COMMITTING,EXPORTS_COMMITTED,EXPORTS_DISCARDED,EXPORTS_PARTIAL };
struct KljRegistrationTransaction {
 pthread_mutex_t mutex;unsigned state;ExportRegistration *first,*last;
};
static void exports_free(JNIEnv *env,ExportRegistration *entry){
 while(entry){ExportRegistration *next=entry->next;
  if(entry->declaring)(*env)->DeleteGlobalRef(env,entry->declaring);
  free(entry->native.name);free(entry->native.signature);free(entry);entry=next;
 }
}
static int exports_declared(JNIEnv *env,jvmtiEnv *ti,jclass target,const char *name,const char *descriptor,jclass *declaring,jmethodID *method){
 jclass current=(*env)->NewLocalRef(env,target);if(!current)return 0;
 while(current){
  jint count=0;jmethodID *methods=NULL;
  if((*ti)->GetClassMethods(ti,current,&count,&methods)!=JVMTI_ERROR_NONE){(*env)->DeleteLocalRef(env,current);return 0;}
  int found=0,okay=0;
  for(jint i=0;i<count;i++){
   char *actual_name=NULL,*actual_descriptor=NULL;
   jvmtiError error=(*ti)->GetMethodName(ti,methods[i],&actual_name,&actual_descriptor,NULL);
   int matches=error==JVMTI_ERROR_NONE&&actual_name&&actual_descriptor&&!strcmp(name,actual_name)&&!strcmp(descriptor,actual_descriptor);
   if(actual_name)(*ti)->Deallocate(ti,(unsigned char*)actual_name);
   if(actual_descriptor)(*ti)->Deallocate(ti,(unsigned char*)actual_descriptor);
   if(matches){
    jint modifiers=0;found=1;
    okay=(*ti)->GetMethodModifiers(ti,methods[i],&modifiers)==JVMTI_ERROR_NONE&&(modifiers&0x0100);
    if(okay){*declaring=(*env)->NewGlobalRef(env,current);*method=methods[i];okay=*declaring!=NULL;}
    break;
   }
  }
  if(methods)(*ti)->Deallocate(ti,(unsigned char*)methods);
  if(found){(*env)->DeleteLocalRef(env,current);return okay;}
  jclass parent=(*env)->GetSuperclass(env,current);(*env)->DeleteLocalRef(env,current);current=parent;
 }
 return 0;
}
static int exports_revalidate(JNIEnv *env,jvmtiEnv *ti,ExportRegistration *entry){
 jint modifiers=0;char *name=NULL,*descriptor=NULL;jclass declaring=NULL;
 int okay=(*ti)->GetMethodModifiers(ti,entry->method,&modifiers)==JVMTI_ERROR_NONE&&(modifiers&0x0100)&&
  (*ti)->GetMethodName(ti,entry->method,&name,&descriptor,NULL)==JVMTI_ERROR_NONE&&name&&descriptor&&
  !strcmp(name,entry->native.name)&&!strcmp(descriptor,entry->native.signature)&&
  (*ti)->GetMethodDeclaringClass(ti,entry->method,&declaring)==JVMTI_ERROR_NONE&&(*env)->IsSameObject(env,entry->declaring,declaring);
 if(name)(*ti)->Deallocate(ti,(unsigned char*)name);
 if(descriptor)(*ti)->Deallocate(ti,(unsigned char*)descriptor);
 if(declaring)(*env)->DeleteLocalRef(env,declaring);return okay;
}
KljRegistrationTransaction *klj_registration_txn_begin(JNIEnv *env){
 if(!env||(*env)->ExceptionCheck(env)||!klj_registration_prepare_facade(env))return NULL;
 KljRegistrationTransaction *transaction=calloc(1,sizeof(*transaction));if(!transaction){registration_failure(env,"Native export preparation allocation failed");return NULL;}
 pthread_mutexattr_t attributes;
 if(pthread_mutexattr_init(&attributes)){free(transaction);registration_failure(env,"Native export preparation lock failed");return NULL;}
 int error=pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE);
 if(!error)error=pthread_mutex_init(&transaction->mutex,&attributes);
 pthread_mutexattr_destroy(&attributes);
 if(error){free(transaction);registration_failure(env,"Native export preparation lock failed");return NULL;}
 transaction->state=EXPORTS_STAGING;return transaction;
}
static jint exports_publish(JNIEnv *env,ExportRegistration *first){
 jvmtiEnv *ti=klj_jni_vm_tooling(env);if(!ti)return JNI_ERR;
 for(ExportRegistration *entry=first;entry;entry=entry->next)if(!exports_revalidate(env,ti,entry)){
  registration_failure(env,"Native export declaration changed during preparation");return JNI_ERR;
 }
 /* The ordinary original JDK method lookup has no failure branch after this
  * complete native-declaration validation. No unknown old binding is read,
  * cleared, or inferred, and no whole-class unregister is issued. */
 for(ExportRegistration *entry=first;entry;entry=entry->next){
  jint result=previous_register(env,entry->declaring,&entry->native,1);
  if(result!=JNI_OK||(*env)->ExceptionCheck(env))return KLJ_REGISTRATION_COMMIT_PARTIAL;
 }
 return JNI_OK;
}
jint klj_registration_txn_stage_exports(JNIEnv *env,KljRegistrationTransaction *transaction,jclass target,const JNINativeMethod *methods,jint count){
 if(!env||!transaction||(*env)->ExceptionCheck(env))return JNI_ERR;
 if(!target||count<0||(count&&!methods)||(size_t)count>SIZE_MAX/sizeof(*methods)){registration_failure(env,"Invalid native export table");return JNI_ERR;}
 jvmtiEnv *ti=klj_jni_vm_tooling(env);if(!ti)return JNI_ERR;
 ExportRegistration *first=NULL,*last=NULL;
 for(jint i=0;i<count;i++){
  if(!methods[i].name||!methods[i].signature||!methods[i].fnPtr){registration_failure(env,"Invalid native export entry");goto failed;}
  ExportRegistration *entry=calloc(1,sizeof(*entry));if(!entry){registration_failure(env,"Native export preparation allocation failed");goto failed;}
  entry->native=(JNINativeMethod){strdup(methods[i].name),strdup(methods[i].signature),methods[i].fnPtr};
  if(!entry->native.name||!entry->native.signature||!exports_declared(env,ti,target,entry->native.name,entry->native.signature,&entry->declaring,&entry->method)){
   exports_free(env,entry);registration_failure(env,"Native export does not match a declared native method");goto failed;
  }
  if(last)last->next=entry;else first=entry;last=entry;
 }
 pthread_mutex_lock(&transaction->mutex);jint result=JNI_OK;
 if(transaction->state==EXPORTS_STAGING){if(transaction->last)transaction->last->next=first;else transaction->first=first;if(last)transaction->last=last;first=NULL;}
 else if(transaction->state==EXPORTS_COMMITTED){
  result=exports_publish(env,first);
  if(result==KLJ_REGISTRATION_COMMIT_PARTIAL){transaction->state=EXPORTS_PARTIAL;transaction->first=first;transaction->last=last;first=NULL;}
 }
 else{registration_failure(env,"Native export preparation is closed");result=JNI_ERR;}
 pthread_mutex_unlock(&transaction->mutex);exports_free(env,first);return result;
failed:
 exports_free(env,first);return JNI_ERR;
}
jint klj_registration_txn_commit(JNIEnv *env,KljRegistrationTransaction *transaction){
 if(!env||!transaction||(*env)->ExceptionCheck(env))return JNI_ERR;
 pthread_mutex_lock(&transaction->mutex);
 if(transaction->state==EXPORTS_COMMITTED){pthread_mutex_unlock(&transaction->mutex);return JNI_OK;}
 if(transaction->state!=EXPORTS_STAGING){pthread_mutex_unlock(&transaction->mutex);return JNI_ERR;}
 transaction->state=EXPORTS_COMMITTING;jint result=exports_publish(env,transaction->first);
 transaction->state=result==JNI_OK?EXPORTS_COMMITTED:result==KLJ_REGISTRATION_COMMIT_PARTIAL?EXPORTS_PARTIAL:EXPORTS_STAGING;
 if(result==JNI_OK){exports_free(env,transaction->first);transaction->first=transaction->last=NULL;}
 pthread_mutex_unlock(&transaction->mutex);return result;
}
jint klj_registration_txn_discard(JNIEnv *env,KljRegistrationTransaction *transaction){
 if(!env||!transaction)return JNI_ERR;pthread_mutex_lock(&transaction->mutex);
 if(transaction->state==EXPORTS_DISCARDED){pthread_mutex_unlock(&transaction->mutex);return JNI_OK;}
 if(transaction->state!=EXPORTS_STAGING){pthread_mutex_unlock(&transaction->mutex);return JNI_ERR;}
 transaction->state=EXPORTS_DISCARDED;ExportRegistration *entries=transaction->first;transaction->first=transaction->last=NULL;
 pthread_mutex_unlock(&transaction->mutex);exports_free(env,entries);return JNI_OK;
}
