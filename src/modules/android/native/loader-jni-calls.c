#include <jni.h>
#include <jvmti.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "jni-invocation.h"
#include "jni-lifetime.h"
extern void klj_enter_guest_thread(void);

typedef struct {uint64_t x[9],lr;unsigned char vectors[8][16];uint64_t stack,integer,floating,operation,reserved[2];} CallFrame;
_Static_assert(sizeof(CallFrame)==256,"JNI assembly frame size");
_Static_assert(offsetof(CallFrame,stack)==208,"JNI assembly stack offset");
static struct JNINativeInterface_ originals;
static jvmtiEnv *runtime_ti;
static JavaVM *lifetime_vm;
static pthread_mutex_t lifetime_mutex=PTHREAD_MUTEX_INITIALIZER;
static KljJniVmObserver *lifetime_observers;
static _Atomic int lifetime_state;
static pthread_mutex_t method_mutex=PTHREAD_MUTEX_INITIALIZER;
typedef struct {jmethodID method;jclass owner;unsigned count;char types[255];} Method;
static Method methods[1024];static unsigned method_count;
static void failure(JNIEnv *env,const char *text){if((*env)->ExceptionCheck(env))return;jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");if(type){(*env)->ThrowNew(env,type,text);(*env)->DeleteLocalRef(env,type);}}
static int signature(JNIEnv *env,jmethodID method,Method *result){
 pthread_mutex_lock(&method_mutex);
 for(unsigned i=0;i<method_count;i++)if(methods[i].method==method){*result=methods[i];pthread_mutex_unlock(&method_mutex);return 1;}
 char *descriptor=NULL;jclass owner=NULL;int cache=method_count<1024;
 int okay=method&&runtime_ti&&(*runtime_ti)->GetMethodName(runtime_ti,method,NULL,&descriptor,NULL)==JVMTI_ERROR_NONE;
 Method entry={.method=method};
 if(okay){size_t length=strlen(descriptor),at=1;okay=descriptor[0]=='(';
  while(okay&&at<length&&descriptor[at]!=')'){
   if(entry.count==255){okay=0;break;}char kind=descriptor[at++];
   if(kind=='['){while(at<length&&descriptor[at]=='[')at++;if(at==length){okay=0;break;}kind=descriptor[at++];if(kind=='L'){while(at<length&&descriptor[at]!=';')at++;if(at==length){okay=0;break;}at++;}else if(!strchr("ZBCSIJFD",kind)){okay=0;break;}kind='L';}
   else if(kind=='L'){while(at<length&&descriptor[at]!=';')at++;if(at==length){okay=0;break;}at++;}
   else if(!strchr("ZBCSIJFD",kind)){okay=0;break;}
   entry.types[entry.count++]=kind;
  }
  okay=okay&&at<length&&descriptor[at]==')';
 }
 if(descriptor)(*runtime_ti)->Deallocate(runtime_ti,(unsigned char*)descriptor);
 if(okay&&cache)okay=(*runtime_ti)->GetMethodDeclaringClass(runtime_ti,method,&owner)==JVMTI_ERROR_NONE;
 if(okay&&cache){entry.owner=(*env)->NewGlobalRef(env,owner);okay=entry.owner!=NULL;}
 if(owner)(*env)->DeleteLocalRef(env,owner);
 if(okay){if(cache)methods[method_count++]=entry;*result=entry;}
 pthread_mutex_unlock(&method_mutex);
 if(!okay)failure(env,"Android JNI method descriptor is unsupported");return okay;
}
unsigned klj_jni_cached_methods(void){pthread_mutex_lock(&method_mutex);unsigned count=method_count;pthread_mutex_unlock(&method_mutex);return count;}
typedef struct {void *stack,*gr_top,*vr_top;int gr_offs,vr_offs;} AndroidVaList;
extern int klj_guest_address(const void *);
static int decode(JNIEnv *env,CallFrame *frame,unsigned fixed,int isV,const Method *method,jvalue *values){
 /* A guest JNIEnv can enter an ordinary native NDK adapter. Its subsequent
  * JNI call uses the adapter's compiler ABI, not the environment's origin.
  * The shared ELF owner identifies Android call sites; Mach-O callers retain
  * Darwin's stack-only variadic convention. No argument-shape guessing. */
 int android=klj_guest_address((void*)(uintptr_t)frame->lr);
 unsigned gp=fixed,fp=0;uintptr_t stack=frame->stack;AndroidVaList ap={0};
 if(isV){if(!frame->x[fixed]){failure(env,"Android JNI argument list is null");return 0;}
  if(android){memcpy(&ap,(void*)frame->x[fixed],sizeof ap);
   if(ap.gr_offs<-64||ap.gr_offs%8||ap.vr_offs<-128||ap.vr_offs%16){failure(env,"Android JNI argument list layout is invalid");return 0;}stack=(uintptr_t)ap.stack;}
  else stack=(uintptr_t)frame->x[fixed];}
 for(unsigned i=0;i<method->count;i++){
  char kind=method->types[i];int floating=kind=='F'||kind=='D';uint64_t bits=0;const void *source=NULL;
  if(android&&isV){if(floating&&ap.vr_offs<0){source=(char*)ap.vr_top+ap.vr_offs;ap.vr_offs+=16;}else if(!floating&&ap.gr_offs<0){source=(char*)ap.gr_top+ap.gr_offs;ap.gr_offs+=8;}}
  else if(android){if(floating&&fp<8)source=frame->vectors[fp++];else if(!floating&&gp<8)source=&frame->x[gp++];}
  if(!source){stack=(stack+7)&~(uintptr_t)7;source=(void*)stack;stack+=8;}
  if(!source){failure(env,"Android JNI argument storage is null");return 0;}memcpy(&bits,source,8);
  switch(kind){case 'Z':values[i].z=(jboolean)bits;break;case 'B':values[i].b=(jbyte)bits;break;case 'C':values[i].c=(jchar)bits;break;case 'S':values[i].s=(jshort)bits;break;case 'I':values[i].i=(jint)bits;break;case 'J':values[i].j=(jlong)bits;break;case 'L':values[i].l=(jobject)(uintptr_t)bits;break;case 'D':memcpy(&values[i].d,&bits,8);break;case 'F':{double promoted;memcpy(&promoted,&bits,8);values[i].f=(jfloat)promoted;break;}}
 }
 return 1;
}
/* Android code receives its own complete JNI tables. A library may interpose
 * them without changing HotSpot, agents, or native macOS JNI callers. Only the
 * environment and ABI representation change; the real VM owns every operation. */
static struct JNINativeInterface_ guest_functions;
typedef struct {const struct JNINativeInterface_ *functions;JNIEnv *host;} GuestEnv;
typedef struct {const struct JNIInvokeInterface_ *functions;JavaVM *host;struct JNIInvokeInterface_ table;} GuestVM;
static pthread_key_t guest_env_key;static pthread_once_t guest_env_once=PTHREAD_ONCE_INIT;
static void create_env_key(void){pthread_key_create(&guest_env_key,free);}
static void release_guest_env(void){pthread_once(&guest_env_once,create_env_key);void *value=pthread_getspecific(guest_env_key);pthread_setspecific(guest_env_key,NULL);free(value);}
static void JNICALL thread_end(jvmtiEnv *ti,JNIEnv *env,jthread thread){
 (void)ti;(void)thread;
 jthrowable pending=(*env)->ExceptionOccurred(env);if(pending)(*env)->ExceptionClear(env);
 klj_jni_thread_retire();
 release_guest_env();
 if(pending){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,pending);(*env)->DeleteLocalRef(env,pending);}
}
static void retire_components(JNIEnv *env,int phase){
 pthread_mutex_lock(&lifetime_mutex);
 KljJniVmObserver *observers=lifetime_observers;
 if(phase==KLJ_JNI_SHUTDOWN){
  if(atomic_load(&lifetime_state)!=1){pthread_mutex_unlock(&lifetime_mutex);return;}
  atomic_store(&lifetime_state,2);
 }
 pthread_mutex_unlock(&lifetime_mutex);
 for(KljJniVmObserver *owner=observers;owner;owner=owner->next)owner->retire(env,phase);
}
static void JNICALL vm_death(jvmtiEnv *ti,JNIEnv *env){
 (void)ti;retire_components(env,KLJ_JNI_VM_DEATH);
 pthread_mutex_lock(&method_mutex);
 for(unsigned i=0;i<method_count;i++)(*env)->DeleteGlobalRef(env,methods[i].owner);
 method_count=0;pthread_mutex_unlock(&method_mutex);
 atomic_store_explicit(&lifetime_state,3,memory_order_release);
}
static int prepare_thread_lifecycle(jvmtiEnv *ti){
 // This is the registration owner's private JVMTI environment. Reuse it;
 // other agents' event callbacks and the native VM table remain untouched.
 jvmtiEventCallbacks callbacks={.ThreadEnd=thread_end,.VMDeath=vm_death};
 return (*ti)->SetEventCallbacks(ti,&callbacks,sizeof callbacks)==JVMTI_ERROR_NONE&&
   (*ti)->SetEventNotificationMode(ti,JVMTI_ENABLE,JVMTI_EVENT_THREAD_END,NULL)==JVMTI_ERROR_NONE&&
   (*ti)->SetEventNotificationMode(ti,JVMTI_ENABLE,JVMTI_EVENT_VM_DEATH,NULL)==JVMTI_ERROR_NONE;
}
static int prepare_call_lifecycle(JNIEnv *env,jvmtiEnv *ti){
 pthread_mutex_lock(&lifetime_mutex);
 int okay=runtime_ti==ti&&atomic_load(&lifetime_state)<3;
 if(!runtime_ti&&!atomic_load(&lifetime_state)){
  JavaVM *vm=NULL;
  okay=(*env)->GetJavaVM(env,&vm)==JNI_OK&&prepare_thread_lifecycle(ti);
  if(okay){lifetime_vm=vm;runtime_ti=ti;atomic_store(&lifetime_state,1);}
 }
 pthread_mutex_unlock(&lifetime_mutex);return okay;
}
jvmtiEnv *klj_jni_vm_tooling(JNIEnv *env){
 pthread_mutex_lock(&lifetime_mutex);
 if(!runtime_ti&&!atomic_load(&lifetime_state)){
  JavaVM *vm=NULL;jvmtiEnv *ti=NULL;
  if((*env)->GetJavaVM(env,&vm)==JNI_OK&&(*vm)->GetEnv(vm,(void**)&ti,JVMTI_VERSION_1_2)==JNI_OK){
   if(prepare_thread_lifecycle(ti)){lifetime_vm=vm;runtime_ti=ti;atomic_store(&lifetime_state,1);}
   else (*ti)->DisposeEnvironment(ti);
  }
 }
 jvmtiEnv *ti=atomic_load(&lifetime_state)<3?runtime_ti:NULL;
 pthread_mutex_unlock(&lifetime_mutex);return ti;
}
int klj_jni_vm_observe(JNIEnv *env,KljJniVmObserver *owner){
 if(!owner||!owner->retire||!klj_jni_vm_tooling(env))return 0;
 pthread_mutex_lock(&lifetime_mutex);
 if(atomic_load(&lifetime_state)!=1){pthread_mutex_unlock(&lifetime_mutex);return 0;}
 for(KljJniVmObserver *entry=lifetime_observers;entry;entry=entry->next)if(entry==owner){pthread_mutex_unlock(&lifetime_mutex);return 1;}
 owner->next=lifetime_observers;lifetime_observers=owner;
 pthread_mutex_unlock(&lifetime_mutex);return 1;
}
JNIEnv *klj_jni_vm_environment(JavaVM *vm){
 if(!vm||vm!=lifetime_vm||atomic_load_explicit(&lifetime_state,memory_order_acquire)==3)return NULL;
 JNIEnv *env=NULL;jint result=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
 if(result==JNI_EDETACHED)result=(*vm)->AttachCurrentThreadAsDaemon(vm,(void**)&env,NULL);
 return result==JNI_OK?env:NULL;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_JvmNativeMemory_retireRuntime(JNIEnv *env,jclass type){(void)type;retire_components(env,KLJ_JNI_SHUTDOWN);}
JNIEnv *klj_host_env(JNIEnv *env){return ((GuestEnv*)env)->host;}
JNIEnv *klj_native_env(JNIEnv *env){
 pthread_once(&guest_env_once,create_env_key);GuestEnv *guest=pthread_getspecific(guest_env_key);
 return guest&&(JNIEnv*)guest==env?guest->host:env;
}
JNIEnv *klj_guest_env(JNIEnv *env){
 pthread_once(&guest_env_once,create_env_key);GuestEnv *guest=pthread_getspecific(guest_env_key);
 if(!guest){guest=calloc(1,sizeof *guest);if(!guest){failure(env,"Android JNI environment allocation failed");return NULL;}guest->functions=&guest_functions;pthread_setspecific(guest_env_key,guest);}
 guest->host=env;return (JNIEnv*)guest;
}
static GuestVM guest_vm;static pthread_mutex_t vm_mutex=PTHREAD_MUTEX_INITIALIZER;
static jint JNICALL guest_vm_DestroyJavaVM(JavaVM *vm){if(atomic_load(&lifetime_state)==3)return JNI_ERR;JavaVM *host=((GuestVM*)vm)->host;return (*host)->DestroyJavaVM(host);}
static jint JNICALL guest_vm_DetachCurrentThread(JavaVM *vm){
 if(atomic_load(&lifetime_state)==3)return JNI_ERR;
 if(klj_jni_thread_retiring())return JNI_OK;
 JavaVM *host=((GuestVM*)vm)->host;jint result=klj_jni_detach_current_thread(host);
 if(result==JNI_OK)release_guest_env();return result;
}
static jint guest_vm_environment(JavaVM *vm,void **out,void *argument,int attach,int daemon){
 // Foreign native callback threads can reach AttachCurrentThread without ever
 // passing the guest pthread entry. Establish the existing native lifetime
 // before the VM attaches; doing this afterwards mistakes them for Java owners.
 if(atomic_load_explicit(&lifetime_state,memory_order_acquire)==3){if(out)*out=NULL;return JNI_ERR;}
 if(attach)klj_enter_guest_thread();
 JavaVM *host=((GuestVM*)vm)->host;void *real=NULL;jint result=attach?(daemon?(*host)->AttachCurrentThreadAsDaemon(host,&real,argument):(*host)->AttachCurrentThread(host,&real,argument)):(*host)->GetEnv(host,&real,(jint)(intptr_t)argument);
 if(result==JNI_OK){int jni=attach||((uint32_t)(uintptr_t)argument&0x70000000u)==0;*out=jni?(void*)klj_guest_env(real):real;if(!*out)return JNI_ENOMEM;}else if(out)*out=NULL;return result;
}
static jint JNICALL guest_vm_GetEnv(JavaVM *vm,void **env,jint version){return guest_vm_environment(vm,env,(void*)(intptr_t)version,0,0);}
static jint JNICALL guest_vm_AttachCurrentThread(JavaVM *vm,void **env,void *args){return guest_vm_environment(vm,env,args,1,0);}
static jint JNICALL guest_vm_AttachCurrentThreadAsDaemon(JavaVM *vm,void **env,void *args){return guest_vm_environment(vm,env,args,1,1);}
JavaVM *klj_guest_vm(JavaVM *vm){
 if(atomic_load_explicit(&lifetime_state,memory_order_acquire)==3)return NULL;
 pthread_mutex_lock(&vm_mutex);if(!guest_vm.host){guest_vm.host=vm;guest_vm.table=**vm;guest_vm.table.DestroyJavaVM=guest_vm_DestroyJavaVM;guest_vm.table.DetachCurrentThread=guest_vm_DetachCurrentThread;guest_vm.table.GetEnv=guest_vm_GetEnv;guest_vm.table.AttachCurrentThread=guest_vm_AttachCurrentThread;guest_vm.table.AttachCurrentThreadAsDaemon=guest_vm_AttachCurrentThreadAsDaemon;guest_vm.functions=&guest_vm.table;}pthread_mutex_unlock(&vm_mutex);return (JavaVM*)&guest_vm;
}
static jint guest_get_vm(JNIEnv *env,JavaVM **vm){JavaVM *host=NULL;jint result=originals.GetJavaVM(klj_host_env(env),&host);if(result==JNI_OK)*vm=klj_guest_vm(host);return result;}
extern jclass klj_find_from_image(JNIEnv*,const char*,uintptr_t);
static jclass guest_find_class(JNIEnv *env,const char *name,void *caller){JNIEnv *host=klj_host_env(env);return klj_find_from_image(host,name,(uintptr_t)caller);}
#include "jni-forward-generated.h"

#include "jni-calls-generated.h"

/* A failed installation never publishes the new table. Retire its provisional
 * metadata environment before the registration owner disposes it. */
void klj_jni_calls_abort(jvmtiEnv *ti){(void)ti;}
