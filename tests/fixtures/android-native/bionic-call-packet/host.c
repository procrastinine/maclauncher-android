#include "admission.h"
#include "call-packet.h"
#include "process-ids.h"
#include "thread-identity.h"
#include <assert.h>
#include <stdatomic.h>
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
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static Prepared prepared;static unsigned command,done,ready;
static _Thread_local uint64_t token,mach_thread;
static _Thread_local JNIEnv *active_environment;
static uint64_t initial_token,retired_token;static jmethodID callback,throw_callback;
static int enrolled_tid;static _Atomic unsigned late_destructors;static pthread_key_t late_key;
static unsigned native_frames,manual_frames,rejected_frames;
#include "call-frame.h"

static uint64_t physical_thread(void){uint64_t value=0;assert(!pthread_threadid_np(0,&value));return value;}
static void retained_identity(void){pid_t host=0;int kind=0,process=0;assert(klj_thread_id()==enrolled_tid&&klj_thread_id_adopt(enrolled_tid)==enrolled_tid);assert(!klj_process_id_query(enrolled_tid,&host,&kind,&process)&&host==getpid()&&kind==2&&process==klj_process_id());}
static void late_destructor(void *raw){assert((intptr_t)raw==enrolled_tid&&state->ended==1);retained_identity();late_destructors++;}
void maclauncher_bionic_admission_install(const KljBionicAdmissionApi *value){assert(value&&value->size==sizeof(*value)&&value->version==KLJ_BIONIC_ADMISSION_ABI&&!api);api=value;}
static void request(unsigned operation){assert(!pthread_mutex_lock(&mutex));command=operation;done=0;pthread_cond_broadcast(&condition);while(!done)pthread_cond_wait(&condition,&mutex);assert(!pthread_mutex_unlock(&mutex));}

/* The admission supplier runs the packet dispatcher on this exact Java pthread.
 * Reentrant Java->JNI calls retain the already active Bionic continuation. */
static int64_t invoke_packet(KljCallPacket *packet,int jni){
 assert(token&&active_environment&&physical_thread()==mach_thread);
 if(jni)assert(packet->x[0]==(uintptr_t)active_environment);
 uint64_t context=api->context(),tp=api->thread_pointer(),result=UINT64_MAX;
 stack_t before={0},after={0};sigset_t mask_before=0,mask_after=0;
 assert(!sigaltstack(0,&before)&&!pthread_sigmask(SIG_SETMASK,0,&mask_before));
 int saved_errno=errno;int64_t status=api->invoke(token,boot.packet,(uintptr_t)packet,&result);
 assert(!status&&(int64_t)result==packet->status&&errno==saved_errno);
 assert(api->context()==context&&api->thread_pointer()==tp&&physical_thread()==mach_thread);
 assert(!sigaltstack(0,&after)&&!pthread_sigmask(SIG_SETMASK,0,&mask_after));
 assert(before.ss_flags==after.ss_flags&&mask_before==mask_after);
 if(!(before.ss_flags&SS_DISABLE))assert(before.ss_sp==after.ss_sp&&before.ss_size==after.ss_size);
 retained_identity();return packet->status;
}
int64_t klj_bionic_call_frame_invoke(KljCallPacket *packet){native_frames++;return invoke_packet(packet,1);}
static uint64_t resolve(const char *name){Lookup lookup={.name=name};uint64_t result=0;assert(!api->invoke(token,boot.resolve,(uintptr_t)&lookup,&result)&&result&&result==lookup.result);return result;}
static KljCallPacket packet_for(uint64_t target){return(KljCallPacket){.size=sizeof(KljCallPacket),.version=KLJ_CALL_PACKET_VERSION,.target=target,.status=-1};}
static void manual_packet_controls(void){
 uint64_t large[3]={0};KljCallPacket packet=packet_for(resolve("fixture_large"));packet.x[0]=10;packet.x[1]=20;packet.x[2]=30;packet.x[8]=(uintptr_t)large;
 assert(!invoke_packet(&packet,0)&&large[0]==11&&large[1]==22&&large[2]==33);manual_frames++;
 packet=packet_for(resolve("fixture_pair"));packet.x[0]=70;packet.x[1]=80;assert(!invoke_packet(&packet,0)&&packet.result_x[0]==73&&packet.result_x[1]==84);manual_frames++;
 packet=packet_for(resolve("fixture_hfa"));float value=2.25f;memcpy(packet.v[0],&value,sizeof value);assert(!invoke_packet(&packet,0));for(unsigned i=0;i<4;i++){float actual;memcpy(&actual,packet.result_v[i],sizeof actual);assert(actual==value+i);}manual_frames++;
 packet=packet_for(resolve("fixture_vector"));uint64_t vector[2]={0x1122334455667788,0x8877665544332211};memcpy(packet.v[0],vector,sizeof vector);assert(!invoke_packet(&packet,0));uint64_t actual[2];memcpy(actual,packet.result_v[0],sizeof actual);assert(actual[0]==vector[0]+3&&actual[1]==vector[1]+5);manual_frames++;
 for(unsigned n=0;n<6;n++){
  packet=packet_for(resolve("fixture_pair"));memset(packet.result_x,0x5a,sizeof packet.result_x);memset(packet.result_v,0x5a,sizeof packet.result_v);
  unsigned char expected[sizeof packet.result_x+sizeof packet.result_v];memset(expected,0x5a,sizeof expected);
  unsigned char stack[16]={0};packet.stack=stack;
  if(n==0)packet.target=0;else if(n==1)packet.size--;else if(n==2)packet.version++;else if(n==3)packet.stack_bytes=8;else if(n==4)packet.stack_bytes=KLJ_PACKET_MAX_STACK+16;else{packet.stack_bytes=16;packet.stack=0;}
  assert(invoke_packet(&packet,0)==-22&&!memcmp(packet.result_x,expected,sizeof expected));rejected_frames++;
 }
 uint64_t result=0;assert(!api->invoke(token,boot.packet,0,&result)&&(int64_t)result==-22);rejected_frames++;
 fprintf(stderr,"packet: real ELF indirect/x0-x1/HFA/q0 returns; seven invalid packet rejections preserve results\n");
}
static void JNICALL prepare_thread(JNIEnv *env,jclass type){
 assert(!token&&!api->context());mach_thread=physical_thread();enrolled_tid=klj_thread_id();assert(enrolled_tid>0);retained_identity();active_environment=env;
 assert(!pthread_key_create(&late_key,late_destructor)&&!pthread_setspecific(late_key,(void*)(intptr_t)enrolled_tid));
 assert(!api->reserve(&token)&&token);prepared=(Prepared){.token=token};request(1);assert(!prepared.error&&prepared.thread);
 struct Method{const char *name,*signature;}cases[]={
  {"roundTrip","(IJFDIIIIII)J"},{"nested","(J)J"},{"narrow","(ZBCSIJIIII)J"},
  {"floating","(FFFFFFFFFDDDDDDDDD)D"},{"object","(Ljava/lang/Object;)Ljava/lang/Object;"},
  {"booleanValue","(Z)Z"},{"byteValue","(B)B"},{"charValue","(C)C"},{"shortValue","(S)S"},
  {"intValue","(I)I"},{"longValue","(J)J"},{"floatValue","(F)F"},{"voidValue","(I)V"},{"throwing","()V"}
 };
 JNINativeMethod methods[sizeof cases/sizeof *cases];
 for(unsigned i=0;i<sizeof cases/sizeof *cases;i++){
  char name[160],error[256]={0};snprintf(name,sizeof name,"Java_BionicProcessProbe_%s",cases[i].name);
  void *target=(void*)(uintptr_t)resolve(name),*entry=klj_bionic_native_abi(target,cases[i].signature,error,sizeof error);if(!entry)fprintf(stderr,"descriptor failure: %s\n",error);assert(entry);
  methods[i]=(JNINativeMethod){(char*)cases[i].name,(char*)cases[i].signature,entry};
 }
 assert((*env)->RegisterNatives(env,type,methods,sizeof methods/sizeof *methods)==JNI_OK);
 manual_packet_controls();assert(state->constructors==1&&state->constructor_tid==(uint64_t)enrolled_tid);
 fprintf(stderr,"packet: fourteen original-descriptor JNI entries registered; same Java Mach thread=%llu enrolledTID=%d\n",(unsigned long long)mach_thread,enrolled_tid);
}
static void JNICALL thread_end(jvmtiEnv *ti,JNIEnv *env,jthread thread){(void)ti;(void)thread;if(!token)return;assert(env==active_environment&&physical_thread()==mach_thread&&!api->context());assert(!api->retire(token));assert(!api->context()&&!api->thread_pointer()&&state->key_destructors==1&&state->cxa_destructors==1);state->ended++;retained_identity();retired_token=token;assert(api->retire(token)<0);token=0;}
int64_t maclauncher_art_host_call(uint32_t function,uint64_t raw,uint64_t length,uint64_t(*guest)(uint64_t,uint64_t)){
 if(function==10){assert(length==sizeof(Bootstrap));boot=*(Bootstrap*)(uintptr_t)raw;state=(State*)(uintptr_t)boot.state;assert(!pthread_mutex_lock(&mutex));ready=1;pthread_cond_broadcast(&condition);for(;;){while(!command)pthread_cond_wait(&condition,&mutex);unsigned operation=command;command=0;assert(!pthread_mutex_unlock(&mutex));
  if(operation==1){assert(!api->arm(prepared.token));assert(!guest(boot.prepare,(uintptr_t)&prepared));assert(!api->disarm(prepared.token));}
  else{assert(operation==2);assert(!guest(boot.join,(uintptr_t)&prepared));}
  assert(!pthread_mutex_lock(&mutex));done=1;pthread_cond_broadcast(&condition);
 }}
 if(function==42){retained_identity();assert(state->constructor_tid==(uint64_t)enrolled_tid&&length==sizeof(State)&&physical_thread()==mach_thread&&active_environment&&api->context()&&api->thread_pointer());return 0;}
 if(function==43||function==44){assert(length==sizeof(JavaCallback)&&physical_thread()==mach_thread);JavaCallback *call=(void*)(uintptr_t)raw;assert(call->env==active_environment);
  if(function==43){call->result=(*call->env)->CallStaticLongMethod(call->env,call->type,callback,call->value);assert(!(*call->env)->ExceptionCheck(call->env));state->callbacks++;}
  else{(*call->env)->CallStaticVoidMethod(call->env,call->type,throw_callback);assert((*call->env)->ExceptionCheck(call->env));}
  return 0;
 }
 return -22;
}
typedef struct{uint32_t size,version;const char *root,*path_map,*program,*runtime,*host_module,*identity;const char *const *envp;size_t envc;}ProcessOptions;
typedef int64_t(*ProcessPrepare)(const ProcessOptions*,uint64_t*,void*);
JNIEXPORT jint JNICALL Java_BionicProcessAgent_initialize(JNIEnv *env,jclass owner,jobjectArray paths){
 (void)owner;assert((*env)->GetArrayLength(env,paths)==7);const char *text[7];jstring values[7];for(int i=0;i<7;i++){values[i]=(*env)->GetObjectArrayElement(env,paths,i);text[i]=(*env)->GetStringUTFChars(env,values[i],0);assert(text[i]);}
 void *abi=dlopen(text[0],RTLD_NOW|RTLD_LOCAL);if(!abi)fprintf(stderr,"libABI load: %s\n",dlerror());assert(abi);ProcessPrepare prepare=dlsym(abi,"klj_bionic_process_prepare");void *signal=dlsym(RTLD_DEFAULT,"JVM_handle_bsd_signal");assert(prepare&&signal);
 extern char **environ;size_t envc=0;while(environ[envc])envc++;ProcessOptions options={sizeof options,1,text[1],text[2],text[3],text[4],text[5],text[6],(const char *const*)environ,envc};int64_t status=prepare(&options,&initial_token,signal);assert(!status&&initial_token&&!api->context());
 for(int i=0;i<7;i++){(*env)->ReleaseStringUTFChars(env,values[i],text[i]);(*env)->DeleteLocalRef(env,values[i]);}
 assert(!pthread_mutex_lock(&mutex));while(!ready)pthread_cond_wait(&condition,&mutex);assert(!pthread_mutex_unlock(&mutex));uint64_t identity=0;assert(!api->invoke(initial_token,boot.initial,0,&identity));assert((identity>>32)==(uint32_t)klj_process_id()&&(uint32_t)identity==(uint32_t)klj_thread_id());
 JavaVM *vm=0;assert((*env)->GetJavaVM(env,&vm)==JNI_OK);jvmtiEnv *ti=0;assert((*vm)->GetEnv(vm,(void**)&ti,JVMTI_VERSION_1_2)==JNI_OK);jvmtiEventCallbacks events={.ThreadEnd=thread_end};assert((*ti)->SetEventCallbacks(ti,&events,sizeof events)==JVMTI_ERROR_NONE&&(*ti)->SetEventNotificationMode(ti,JVMTI_ENABLE,JVMTI_EVENT_THREAD_END,0)==JVMTI_ERROR_NONE);
 jclass type=(*env)->FindClass(env,"BionicProcessProbe");assert(type);callback=(*env)->GetStaticMethodID(env,type,"callback","(J)J");throw_callback=(*env)->GetStaticMethodID(env,type,"throwCallback","()V");assert(callback&&throw_callback);JNINativeMethod methods[]={{"prepare","()V",(void*)prepare_thread}};assert((*env)->RegisterNatives(env,type,methods,1)==JNI_OK);return 0;
}
JNIEXPORT void JNICALL Java_BionicProcessProbe_complete(JNIEnv *env,jclass type){
 (void)env;(void)type;assert(retired_token&&state->ended==1&&state->calls==4&&state->nested==2&&state->callbacks==2&&native_frames==27&&manual_frames==4&&rejected_frames==7);request(2);
 for(unsigned n=0;n<2000;n++){thread_t port=klj_thread_acquire(enrolled_tid);if(!port)break;mach_port_deallocate(mach_task_self(),port);usleep(1000);}
 assert(late_destructors==1&&!klj_thread_acquire(enrolled_tid));
 fprintf(stderr,"packet: %u JNI frames, %u aggregate/vector frames, %u rejections; original pthread/C++ destructors exactly once; late identity retained until Mach death\n",native_frames,manual_frames,rejected_frames);
}
