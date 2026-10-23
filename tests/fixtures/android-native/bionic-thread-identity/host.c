#include "admission.h"
#include <assert.h>
#include <stdatomic.h>
#include "process-ids.h"
#include "thread-identity.h"
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <jvmti.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const KljBionicAdmissionApi *api;
static Bootstrap boot;static State *state;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static Prepared prepared;static unsigned command,done,ready,stop;
static _Thread_local uint64_t token,entry,mach_thread;
static _Thread_local JNIEnv *active_environment;
static uint64_t retired_token;static jmethodID callback;
static int enrolled_tid;static _Atomic unsigned late_destructors;static pthread_key_t late_key;
static void retained_identity(void){pid_t host=0;int kind=0,process=0;assert(klj_thread_id()==enrolled_tid&&klj_thread_id_adopt(enrolled_tid)==enrolled_tid);assert(!klj_process_id_query(enrolled_tid,&host,&kind,&process)&&host==getpid()&&kind==2&&process==klj_process_id());}
static void late_destructor(void *raw){assert((intptr_t)raw==enrolled_tid&&state->ended==1);retained_identity();late_destructors++;}

void maclauncher_bionic_admission_install(const KljBionicAdmissionApi *value){assert(value&&value->size==sizeof(*value)&&value->version==KLJ_BIONIC_ADMISSION_ABI&&!api);api=value;fputs("admission: original API installed\n",stderr);}
static uint64_t physical_thread(void){uint64_t value=0;assert(!pthread_threadid_np(0,&value));return value;}
static void request(unsigned operation){assert(!pthread_mutex_lock(&mutex));command=operation;done=0;pthread_cond_broadcast(&condition);while(!done)pthread_cond_wait(&condition,&mutex);assert(!pthread_mutex_unlock(&mutex));}
static jlong JNICALL nested(JNIEnv *env,jclass type,jlong value){(void)type;assert(token&&env==active_environment&&physical_thread()==mach_thread&&api->context()&&api->thread_pointer());uint64_t result=0;assert(!api->invoke(token,boot.nested,value,&result));return result;}
static jlong JNICALL round_trip(JNIEnv *env,jclass type,jint a,jlong b,jfloat c,jdouble d,jint e,jint f,jint g,jint h,jint i,jint j){
 fputs("admission: actual Java Thread native entry\n",stderr);assert(!api->context());if(!token){mach_thread=physical_thread();enrolled_tid=klj_thread_id();assert(enrolled_tid>0);retained_identity();assert(!pthread_key_create(&late_key,late_destructor)&&!pthread_setspecific(late_key,(void*)(intptr_t)enrolled_tid));assert(!api->reserve(&token)&&token);retained_identity();prepared=(Prepared){.token=token};request(1);assert(!prepared.error&&prepared.thread);}
 assert(physical_thread()==mach_thread);active_environment=env;
 stack_t before={0},after={0};sigset_t mask_before=0,mask_after=0;assert(!sigaltstack(0,&before)&&!pthread_sigmask(SIG_SETMASK,0,&mask_before));
 fprintf(stderr,"admission: native altstack flags=%d size=%zu pointer=%p\n",before.ss_flags,before.ss_size,before.ss_sp);
 uint64_t rejected=UINT64_C(0x1234);errno=311;(void)rejected;
 Typed call={.function=entry,.env=env,.type=type,.values={{.i=a},{.j=b},{.f=c},{.d=d},{.i=e},{.i=f},{.i=g},{.i=h},{.i=i},{.i=j}}};
 
 uint64_t result=0;int64_t status=api->invoke(token,boot.dispatch,(uintptr_t)&call,&result);fprintf(stderr,"admission: invoke status=%lld calls=%u nested=%u callbacks=%u context=%llu tp=%llu\n",(long long)status,state->calls,state->nested,state->callbacks,(unsigned long long)api->context(),(unsigned long long)api->thread_pointer());assert(!status);entry=result;retained_identity();assert(state->constructor_tid==(uint64_t)enrolled_tid);request(3);retained_identity();
 
 assert(entry&&errno==311&&!api->context()&&!api->thread_pointer()&&physical_thread()==mach_thread);
 assert(!sigaltstack(0,&after)&&!pthread_sigmask(SIG_SETMASK,0,&mask_after));assert(before.ss_flags==after.ss_flags&&mask_before==mask_after);if(!(before.ss_flags&SS_DISABLE))assert(before.ss_sp==after.ss_sp&&before.ss_size==after.ss_size);return call.result;
}
static void JNICALL thread_end(jvmtiEnv *ti,JNIEnv *env,jthread thread){(void)ti;(void)thread;if(!token)return;assert(env==active_environment&&physical_thread()==mach_thread&&!api->context());assert(!api->retire(token));assert(!api->context()&&!api->thread_pointer()&&state->key_destructors==1&&state->cxa_destructors==1);state->ended++;retained_identity();retired_token=token;assert(api->retire(token)<0);token=0;}
static int run_vm(void){
 fputs("admission: create JVM entry\n",stderr);assert(api);assert(!pthread_mutex_lock(&mutex));while(!ready)pthread_cond_wait(&condition,&mutex);assert(!pthread_mutex_unlock(&mutex));
 const char *library=getenv("KLJ_BIONIC_TEST_JVM"),*classes=getenv("KLJ_BIONIC_TEST_CLASSES");assert(library&&classes);fputs("admission: libjvm dlopen begin\n",stderr);void *handle=dlopen(library,RTLD_NOW|RTLD_LOCAL);assert(handle);fputs("admission: libjvm dlopen complete\n",stderr);jint(*create)(JavaVM**,void**,void*)=dlsym(handle,"JNI_CreateJavaVM");assert(create);
 char classpath[4096];assert(snprintf(classpath,sizeof classpath,"-Djava.class.path=%s",classes)<(int)sizeof classpath);
 JavaVMOption options[]={{classpath,0},{"-Xcheck:jni",0},{"-Xmx64m",0},{"-XX:+UseSerialGC",0},{"-XX:ActiveProcessorCount=2",0},{"-Xss512k",0},{"-Xrs",0}};JavaVMInitArgs arguments={JNI_VERSION_1_8,sizeof options/sizeof options[0],options,JNI_FALSE};JavaVM *vm=0;JNIEnv *env=0;assert(create(&vm,(void**)&env,&arguments)==JNI_OK);fputs("admission: JVM created\n",stderr);
 jvmtiEnv *ti=0;assert((*vm)->GetEnv(vm,(void**)&ti,JVMTI_VERSION_1_2)==JNI_OK);jvmtiEventCallbacks events={.ThreadEnd=thread_end};assert((*ti)->SetEventCallbacks(ti,&events,sizeof events)==JVMTI_ERROR_NONE&&(*ti)->SetEventNotificationMode(ti,JVMTI_ENABLE,JVMTI_EVENT_THREAD_END,0)==JVMTI_ERROR_NONE);
 jclass type=(*env)->FindClass(env,"BionicAdmissionProbe");assert(type);callback=(*env)->GetStaticMethodID(env,type,"callback","(J)J");assert(callback);JNINativeMethod methods[]={{"roundTrip","(IJFDIIIIII)J",(void*)round_trip},{"nested","(J)J",(void*)nested}};assert((*env)->RegisterNatives(env,type,methods,2)==JNI_OK);jmethodID run=(*env)->GetStaticMethodID(env,type,"run","()V");assert(run);(*env)->CallStaticVoidMethodA(env,type,run,0);jthrowable failure=(*env)->ExceptionOccurred(env);if(failure)(*env)->ExceptionDescribe(env);assert(!failure&&retired_token&&state->ended==1);
 request(2);for(unsigned n=0;n<2000;n++){thread_t port=klj_thread_acquire(enrolled_tid);if(!port)break;mach_port_deallocate(mach_task_self(),port);usleep(1000);}assert(late_destructors==1&&!klj_thread_acquire(enrolled_tid));pid_t physical=0;int kind=0,process=0;assert(klj_process_id_query(enrolled_tid,&physical,&kind,&process)<0);fprintf(stderr,"identity: enrolled/guest TID=%d late_destructors=%u retired after Mach death\n",enrolled_tid,late_destructors);assert((*vm)->DestroyJavaVM(vm)==JNI_OK);assert(!pthread_mutex_lock(&mutex));stop=1;pthread_cond_broadcast(&condition);assert(!pthread_mutex_unlock(&mutex));return 0;
}
int64_t maclauncher_art_host_call(uint32_t function,uint64_t raw,uint64_t length,uint64_t(*guest)(uint64_t,uint64_t)){
 if(function==10){assert(length==sizeof(Bootstrap));boot=*(Bootstrap*)(uintptr_t)raw;state=(State*)(uintptr_t)boot.state;assert(!pthread_mutex_lock(&mutex));ready=1;pthread_cond_broadcast(&condition);for(;;){while(!command&&!stop)pthread_cond_wait(&condition,&mutex);if(stop)break;unsigned operation=command;command=0;assert(!pthread_mutex_unlock(&mutex));
  if(operation==1){assert(!api->arm(prepared.token));assert(!guest(boot.prepare,(uintptr_t)&prepared));assert(!api->disarm(prepared.token));uint64_t result=91;assert(api->invoke(prepared.token,boot.nested,0,&result)<0&&result==91);}
  else if(operation==2){assert(!guest(boot.join,(uintptr_t)&prepared));}
  else{assert(operation==3);Prepared collision={.token=prepared.token};assert(!api->arm(collision.token));uint64_t failure=guest(boot.prepare,(uintptr_t)&collision);assert(failure==16&&collision.error==16&&!collision.thread);assert(!api->disarm(collision.token));fprintf(stderr,"identity: live original Thread map collision rejected EBUSY before publication\n");}
  assert(!pthread_mutex_lock(&mutex));done=1;pthread_cond_broadcast(&condition);
 }assert(!pthread_mutex_unlock(&mutex));return 0;}
 if(function==11){assert(length==sizeof(Bootstrap));return run_vm();}
 if(function==42){retained_identity();assert(state->constructor_tid==(uint64_t)enrolled_tid);assert(length==sizeof(State)&&physical_thread()==mach_thread&&active_environment&&api->context()&&api->thread_pointer());return 0;}
 if(function==43){assert(length==sizeof(JavaCallback)&&physical_thread()==mach_thread);JavaCallback *call=(void*)(uintptr_t)raw;assert(call->env==active_environment);call->result=(*call->env)->CallStaticLongMethod(call->env,call->type,callback,call->value);assert(!(*call->env)->ExceptionCheck(call->env));state->callbacks++;return 0;}
 return -22;
}
