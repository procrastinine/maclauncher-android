#include "jni-carrier.h"
#include "jni-provider.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
extern JNIEnv *klj_guest_env(JNIEnv *);
extern JNIEnv *klj_host_env(JNIEnv *);
extern JavaVM *klj_guest_vm(JavaVM *);
extern jclass klj_find_from_image(JNIEnv *,const char *,uintptr_t);
typedef struct GuestEnvironment {
 const struct JNINativeInterface_ *functions;
 JNIEnv *facade;KljBionicJniOwner *owner;
 struct GuestEnvironment *next;
} GuestEnvironment;
typedef struct {
 const struct JNIInvokeInterface_ *functions;
 JavaVM *facade;KljBionicJniOwner *owner;
} GuestVM;
struct KljBionicJniOwner {
 KljBionicJniGuestTables tables;GuestVM vm;
 int(*prepare_attach)(void *);void *context;
};
typedef struct CallScope {struct CallScope *previous;KljBionicJniOwner *owner;uintptr_t caller;} CallScope;
static _Thread_local CallScope *current;
static pthread_key_t environment_key;static pthread_once_t environment_once=PTHREAD_ONCE_INIT;
static void retire_environments(void *raw){GuestEnvironment *entry=raw;while(entry){GuestEnvironment *next=entry->next;free(entry);entry=next;}}
static void make_key(void){(void)pthread_key_create(&environment_key,retire_environments);}
static GuestEnvironment *find_environment(KljBionicJniOwner *owner){pthread_once(&environment_once,make_key);for(GuestEnvironment *env=pthread_getspecific(environment_key);env;env=env->next)if(env->owner==owner)return env;return NULL;}
JNIEnv *klj_bionic_jni_environment(KljBionicJniOwner *owner,JNIEnv *real){
 if(!owner||!real)return NULL;JNIEnv *facade=klj_guest_env(real);if(!facade)return NULL;
 GuestEnvironment *entry=find_environment(owner);if(!entry){entry=calloc(1,sizeof(*entry));if(!entry)return NULL;*entry=(GuestEnvironment){owner->tables.jni,facade,owner,pthread_getspecific(environment_key)};if(pthread_setspecific(environment_key,entry)){free(entry);return NULL;}}
 entry->facade=facade;return(JNIEnv *)entry;
}
JNIEnv *klj_bionic_jni_host_environment(JNIEnv *raw){GuestEnvironment *entry=current?find_environment(current->owner):NULL;return entry&&(JNIEnv *)entry==raw?entry->facade:NULL;}
JavaVM *klj_bionic_jni_host_vm(JavaVM *raw){return current&&raw==(JavaVM *)&current->owner->vm?current->owner->vm.facade:NULL;}
JNIEnv *klj_bionic_jni_wrap_environment(JNIEnv *facade){
 if(!current||!facade)return NULL;return klj_bionic_jni_environment(current->owner,klj_host_env(facade));
}
JavaVM *klj_bionic_jni_wrap_vm(JavaVM *facade){return current&&facade==current->owner->vm.facade?(JavaVM *)&current->owner->vm:NULL;}
JavaVM *klj_bionic_jni_vm(KljBionicJniOwner *owner){return owner?(JavaVM *)&owner->vm:NULL;}
KljBionicJniOwner *klj_bionic_jni_owner(JNIEnv *env,const KljBionicJniGuestTables *tables,int(*prepare_attach)(void *),void *context){
 if(!env||!tables||!tables->jni||!tables->vm||!prepare_attach||!klj_registration_prepare_facade(env))return NULL;
 JavaVM *vm=NULL;if((*env)->GetJavaVM(env,&vm)!=JNI_OK)return NULL;JavaVM *facade=klj_guest_vm(vm);if(!facade)return NULL;
 KljBionicJniOwner *owner=calloc(1,sizeof(*owner));if(!owner)return NULL;owner->tables=*tables;owner->prepare_attach=prepare_attach;owner->context=context;owner->vm=(GuestVM){tables->vm,facade,owner};return owner;
}
int klj_bionic_jni_prepare_attach(void *raw){return current&&raw==(JavaVM *)&current->owner->vm?current->owner->prepare_attach(current->owner->context):-1;}
uintptr_t klj_bionic_jni_caller(void){return current?current->caller:0;}
jclass klj_bionic_jni_find_class(JNIEnv *facade,const char *name,uintptr_t caller){
 JNIEnv *env=klj_host_env(facade);if(!klj_jni_before_java(env))return NULL;jclass result=klj_find_from_image(env,name,caller);klj_jni_after_java(env);return result;
}
int64_t klj_bionic_jni_host_call(KljBionicJniOwner *owner,KljBionicJniPacket *packet){
 /* The original host-module callback has already installed its real host
  * stack/TLS. The original ELF caller is retained independently of this C PC. */
 if(!owner||!packet||!klj_jni_provider_address((void*)(uintptr_t)packet->caller))return -22;
 CallScope scope={current,owner,packet->caller};current=&scope;int64_t status=klj_bionic_jni_dispatch_packet(packet);current=scope.previous;return status;
}
