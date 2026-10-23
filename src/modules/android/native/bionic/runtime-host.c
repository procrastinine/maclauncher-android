#include "runtime-host.h"
#include "thread-admission.h"
#include "jni-invocation.h"
#include "thread-identity.h"
#include "process-ids.h"
#include <dlfcn.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PREPARING=1, READY, DRAINING, CLOSED, FAILED };
enum { REQUEST_PREPARE=1, REQUEST_JOIN, REQUEST_STOP };
typedef uint64_t (*GuestCall)(uint64_t,uint64_t);
typedef struct Request {
 unsigned operation,done;int64_t status;
 KljBionicThreadRequest thread;struct Request *next;
} Request;
typedef struct RuntimeThread {
 KljBionicRuntime *runtime;uint64_t token,thread;JNIEnv *environment;
 unsigned initial,depth,retiring,native_owner;
} RuntimeThread;
typedef struct HostScope {KljBionicRuntime *runtime;GuestCall guest;struct HostScope *previous;} HostScope;
typedef struct EnvironmentScope {JNIEnv *environment;struct EnvironmentScope *previous;} EnvironmentScope;
struct KljBionicRuntime {
 pthread_mutex_t mutex;pthread_cond_t condition;
 unsigned phase,service_ready,ordinary_threads,active_entries;
 int64_t failure;Request *head,*tail;
 const KljBionicAdmissionApi *admission;
 void *abi;JavaVM *vm;pthread_t first_thread;uint64_t initial_token;
 KljBionicGuest guest;KljBionicJniOwner *jni;
 KljJniThreadObserver observer;
 KljBionicPreparedCallback prepared;void *prepared_owner;
 KljBionicHostCallback host_callback;void *host_owner;
};
static _Atomic(KljBionicRuntime *) current_runtime;
static _Thread_local RuntimeThread *current_thread;
static _Thread_local HostScope *host_scope;
static _Thread_local EnvironmentScope *environment_scope;
static void retire_native_thread(void *);

static void fail_locked(KljBionicRuntime *runtime,int64_t error){
 if(!runtime->failure)runtime->failure=error?error:-5;
 pthread_cond_broadcast(&runtime->condition);
}
static void fail(KljBionicRuntime *runtime,int64_t error){
 pthread_mutex_lock(&runtime->mutex);fail_locked(runtime,error);pthread_mutex_unlock(&runtime->mutex);
}
static int64_t enqueue(KljBionicRuntime *runtime,Request *request){
 pthread_mutex_lock(&runtime->mutex);
 if(!runtime->service_ready||runtime->failure){int64_t status=runtime->failure?runtime->failure:-108;pthread_mutex_unlock(&runtime->mutex);return status;}
 if(runtime->tail)runtime->tail->next=request;else runtime->head=request;
 runtime->tail=request;pthread_cond_broadcast(&runtime->condition);
 /* The queued request borrows its caller's stack until the original Bionic
  * coordinator completes it. Never abandon that memory on a timeout. */
 while(!request->done)pthread_cond_wait(&runtime->condition,&runtime->mutex);
 int64_t result=request->status;pthread_mutex_unlock(&runtime->mutex);return result;
}
void maclauncher_bionic_admission_install(const KljBionicAdmissionApi *api){
 KljBionicRuntime *runtime=atomic_load_explicit(&current_runtime,memory_order_acquire);
 if(!runtime)return;
 if(!api||api->size!=sizeof(*api)||api->version!=KLJ_BIONIC_ADMISSION_ABI||
    !api->reserve||!api->arm||!api->disarm||!api->invoke||!api->retire||!api->context||!api->thread_pointer){fail(runtime,-22);return;}
 pthread_mutex_lock(&runtime->mutex);
 if(runtime->admission&&runtime->admission!=api)fail_locked(runtime,-16);else runtime->admission=api;
 pthread_mutex_unlock(&runtime->mutex);
}
static int64_t serve(KljBionicRuntime *runtime,GuestCall guest){
 pthread_mutex_lock(&runtime->mutex);runtime->service_ready=1;pthread_cond_broadcast(&runtime->condition);
 for(;;){
  while(!runtime->head)pthread_cond_wait(&runtime->condition,&runtime->mutex);
  Request *request=runtime->head;runtime->head=request->next;if(!runtime->head)runtime->tail=NULL;
  pthread_mutex_unlock(&runtime->mutex);int64_t status=0;
  if(request->operation==REQUEST_PREPARE){
   status=runtime->admission->arm(request->thread.token);
   if(!status){
    uint64_t result=guest(runtime->guest.prepare,(uintptr_t)&request->thread);
    int64_t disarmed=runtime->admission->disarm(request->thread.token);
    status=result?(int64_t)result:disarmed;
    if(!status&&request->thread.error)status=-request->thread.error;
   }
  }else if(request->operation==REQUEST_JOIN){status=(int64_t)guest(runtime->guest.join,(uintptr_t)&request->thread);}
  else if(request->operation!=REQUEST_STOP)status=-22;
  pthread_mutex_lock(&runtime->mutex);request->status=status;request->done=1;
  if(request->operation==REQUEST_STOP){runtime->service_ready=0;pthread_cond_broadcast(&runtime->condition);pthread_mutex_unlock(&runtime->mutex);return 0;}
  pthread_cond_broadcast(&runtime->condition);
 }
}
static JNIEnv *real_environment(KljBionicRuntime *runtime){
 if(environment_scope)return environment_scope->environment;
 JNIEnv *env=NULL;return runtime->vm&&(*runtime->vm)->GetEnv(runtime->vm,(void**)&env,JNI_VERSION_1_6)==JNI_OK?env:NULL;
}
int64_t maclauncher_art_host_call(uint32_t function,uint64_t raw,uint64_t length,GuestCall guest){
 KljBionicRuntime *runtime=atomic_load_explicit(&current_runtime,memory_order_acquire);
 if(!runtime||!runtime->admission||!guest)return -22;
 HostScope scope={runtime,guest,host_scope};host_scope=&scope;int64_t result=-38;
 if(function==KLJ_BIONIC_BOOTSTRAP_HOSTCALL){
  const KljBionicGuest *boot=(const void*)(uintptr_t)raw;
  if(length!=sizeof(*boot)||!boot||boot->size!=sizeof(*boot)||boot->version!=KLJ_BIONIC_RUNTIME_ABI||
     !boot->prepare||!boot->join||!boot->packet||!boot->jni_install||!boot->jni_tables||!boot->try_join_coordinator||
     boot->pid!=(uint64_t)klj_process_id()||!boot->tid||!boot->coordinator_tid){result=-22;fail(runtime,result);}
  else{
   pthread_mutex_lock(&runtime->mutex);int duplicate=runtime->guest.size!=0;
   if(!duplicate)runtime->guest=*boot;pthread_mutex_unlock(&runtime->mutex);
   if(duplicate)result=-16;else result=serve(runtime,guest);
  }
 }else if(!runtime->admission->context()||!runtime->admission->thread_pointer()){result=-1;}
 else if(function==KLJ_BIONIC_JNI_HOSTCALL){
  if(length==sizeof(KljBionicJniPacket)&&raw&&runtime->jni)result=klj_bionic_jni_host_call(runtime->jni,(void*)(uintptr_t)raw);else result=-22;
 }else if(function==KLJ_BIONIC_PREPARED_HOSTCALL){
  KljBionicPreparedEvent *event=(void*)(uintptr_t)raw;KljBionicPreparedCallback callback;void *owner;
  pthread_mutex_lock(&runtime->mutex);callback=runtime->prepared;owner=runtime->prepared_owner;pthread_mutex_unlock(&runtime->mutex);
  JNIEnv *env=real_environment(runtime);
  if(length!=sizeof(*event)||!event||event->size!=sizeof(*event)||event->version!=KLJ_BIONIC_RUNTIME_ABI||!env)result=0;
  else if(!callback){if(event->error&&event->error_capacity)snprintf(event->error,event->error_capacity,"Native image preparation is not registered");result=0;}
  else result=callback(owner,runtime,env,event->context,event->graph,event->error,event->error_capacity,NULL)?1:0;
 }else if(function==KLJ_BIONIC_LINKER_HOSTCALL){
  KljBionicLinkerEvent *event=(void*)(uintptr_t)raw;KljBionicPreparedCallback callback;void *owner;
  pthread_mutex_lock(&runtime->mutex);callback=runtime->prepared;owner=runtime->prepared_owner;pthread_mutex_unlock(&runtime->mutex);
  JNIEnv *env=real_environment(runtime);
  if(length!=sizeof(*event)||!event||event->size!=sizeof(*event)||event->version!=KLJ_BIONIC_RUNTIME_ABI||event->reserved||event->phase<MACLAUNCHER_DLOPEN_PREPARE||event->phase>MACLAUNCHER_DLOPEN_RETIRE||!callback)result=0;
  else result=callback(owner,runtime,env,NULL,event->graph,event->error,event->error_capacity,event)?1:0;
 }else{
  KljBionicHostCallback callback;void *owner;
  pthread_mutex_lock(&runtime->mutex);callback=runtime->host_callback;owner=runtime->host_owner;pthread_mutex_unlock(&runtime->mutex);
  JNIEnv *env=real_environment(runtime);
  /* A genuine Bionic pthread may use pure C Android APIs without joining
   * the JVM. The callback receives the actual (possibly absent) environment;
   * APIs with explicit JNI arguments still validate it at their own entry. */
  if(callback)result=callback(owner,runtime,env,function,raw,length);
 }
 host_scope=scope.previous;return result;
}
int64_t klj_bionic_runtime_host_callback(KljBionicRuntime *runtime,KljBionicHostCallback callback,void *owner){
 if(!runtime||!callback)return -22;
 pthread_mutex_lock(&runtime->mutex);
 int64_t result=runtime->host_callback&&(runtime->host_callback!=callback||runtime->host_owner!=owner)?-16:0;
 if(!result){runtime->host_callback=callback;runtime->host_owner=owner;}
 pthread_mutex_unlock(&runtime->mutex);return result;
}
static int prepare_attachment(void *owner){
 KljBionicRuntime *runtime=owner;
 return host_scope&&host_scope->runtime==runtime&&runtime->admission->context()&&runtime->admission->thread_pointer()?0:-1;
}
static int64_t ensure_thread(KljBionicRuntime *runtime,JNIEnv *env,RuntimeThread **output,int native_owner){
 if(current_thread){
  if(current_thread->runtime!=runtime||current_thread->retiring)return -16;
  current_thread->environment=env;*output=current_thread;return 0;
 }
 if(klj_thread_id()<=0)return -1;
 RuntimeThread *thread=calloc(1,sizeof(*thread));if(!thread)return -12;
 thread->runtime=runtime;thread->environment=env;thread->native_owner=native_owner;
 int64_t status=runtime->admission->reserve(&thread->token);
 if(!status){
  Request request={.operation=REQUEST_PREPARE,.thread={.token=thread->token}};status=enqueue(runtime,&request);thread->thread=request.thread.thread;
  if(!status&&!thread->thread)status=-5;
 }
 if(status){
  if(thread->thread){current_thread=thread;pthread_mutex_lock(&runtime->mutex);runtime->ordinary_threads++;fail_locked(runtime,status);pthread_mutex_unlock(&runtime->mutex);}
  else{if(thread->token){int64_t retired=runtime->admission->retire(thread->token);if(retired)fail(runtime,retired);}free(thread);}
  return status;
 }
 current_thread=thread;
 /* Host media feeders keep one original Bionic pthread across callbacks.
  * Its physical-thread lifetime is independent of Java attachment. Register
  * after the host owner's thread setup so this retires before that setup. */
 if(native_owner)_tlv_atexit(retire_native_thread,runtime);
 pthread_mutex_lock(&runtime->mutex);runtime->ordinary_threads++;pthread_mutex_unlock(&runtime->mutex);*output=thread;return 0;
}
static int enter_owned(void *raw){(void)raw;return 0;}
static int64_t invoke(KljBionicRuntime *runtime,JNIEnv *env,uint64_t function,uint64_t argument,uint64_t *result,int draining,int native_entry){
 if(!runtime||!function||!result)return -22;
 int saved_errno=errno;int nested=host_scope&&host_scope->runtime==runtime&&runtime->admission->context()&&runtime->admission->thread_pointer();
 /* Ordinary NULL-env entry remains nested-only. An authenticated native
  * callback has a separate entry and keeps the physical feeder detached. */
 if(!env&&((!nested&&!native_entry)||real_environment(runtime)))return -22;
 if(!env&&!nested&&current_thread&&!current_thread->native_owner)return -22;
 pthread_mutex_lock(&runtime->mutex);
 int64_t status=runtime->failure?runtime->failure:((runtime->phase==READY||(draining&&runtime->phase==DRAINING)||(nested&&runtime->phase!=FAILED&&runtime->phase!=CLOSED))?0:-108);
 if(!status)runtime->active_entries++;pthread_mutex_unlock(&runtime->mutex);
 if(status){errno=saved_errno;return status;}
 RuntimeThread *thread=NULL;
 if(!nested)status=ensure_thread(runtime,env,&thread,native_entry&&!env);
 KljJniInvocation invocation;int entered=0;
 EnvironmentScope scope={env,environment_scope};if(env)environment_scope=&scope;
 if(!status&&!env){
  if(nested)*result=host_scope->guest(function,argument);
  else{thread->depth++;status=runtime->admission->invoke(thread->token,function,argument,result);thread->depth--;}
 }else if(!status){
  entered=klj_jni_invocation_enter_owned(&invocation,env,enter_owned,runtime);
  if(!entered)status=-5;
  else if(nested)*result=host_scope->guest(function,argument);
  else{thread->depth++;status=runtime->admission->invoke(thread->token,function,argument,result);thread->depth--;}
  klj_jni_invocation_leave(&invocation);
 }
 if(env)environment_scope=scope.previous;
 pthread_mutex_lock(&runtime->mutex);runtime->active_entries--;pthread_cond_broadcast(&runtime->condition);pthread_mutex_unlock(&runtime->mutex);
 errno=saved_errno;return status;
}
int64_t klj_bionic_runtime_invoke(KljBionicRuntime *runtime,JNIEnv *env,uint64_t function,uint64_t argument,uint64_t *result){return invoke(runtime,env,function,argument,result,0,0);}
static void retire_owned_thread(void *owner){
 KljBionicRuntime *runtime=owner;RuntimeThread *thread=current_thread;
 if(!thread||thread->runtime!=runtime||thread->initial)return;
 if(thread->depth||thread->retiring){fail(runtime,-16);return;}
 thread->retiring=1;EnvironmentScope scope={thread->environment,environment_scope};environment_scope=&scope;
 int64_t status=runtime->admission->retire(thread->token);
 if(!status){Request request={.operation=REQUEST_JOIN,.thread={.token=thread->token,.thread=thread->thread}};status=enqueue(runtime,&request);}
 environment_scope=scope.previous;
 if(status){fail(runtime,status);fprintf(stderr,"Bionic native thread retirement failed: %lld\n",(long long)status);return;}
 current_thread=NULL;pthread_mutex_lock(&runtime->mutex);runtime->ordinary_threads--;pthread_cond_broadcast(&runtime->condition);pthread_mutex_unlock(&runtime->mutex);free(thread);
}
static void retire_java_thread(void *owner){
 if(current_thread&&current_thread->native_owner)return;
 retire_owned_thread(owner);
}
static void retire_native_thread(void *owner){
 KljBionicRuntime *runtime=owner;
 if(!current_thread||current_thread->runtime!=runtime||!current_thread->native_owner)return;
 /* Explicit guest Attach/Detach may have changed the VM attachment since the
  * last callback. Query the actual thread rather than retaining an old Env. */
 current_thread->environment=real_environment(runtime);
 retire_owned_thread(owner);
}
typedef struct {uint32_t size,version;const char *root,*path_map,*program,*runtime,*host_module,*identity;const char *const *envp;size_t envc;} ProcessOptions;
typedef int64_t(*ProcessPrepare)(const ProcessOptions*,uint64_t*,void*);
int64_t klj_bionic_runtime_prepare(JNIEnv *env,const KljBionicRuntimeOptions *options,KljBionicRuntime **output){
 if(!env||!options||!output||options->size!=sizeof(*options)||options->version!=KLJ_BIONIC_RUNTIME_ABI||!options->abi||!options->root||!options->path_map||!options->program||!options->runtime||!options->host_module||!options->identity||(options->envc&&!options->envp))return -22;
 *output=NULL;
 /* The original module initialization configures the canonical ID directory.
  * Its process_prepare then checks leader/TID equality before guest entry.
  * Querying that namespace before module initialization would reject its own
  * first legitimate prepare with ENXIO. Never adopt or allocate a substitute. */
 if(!pthread_main_np())return -1;
 KljBionicRuntime *runtime=calloc(1,sizeof(*runtime));if(!runtime)return -12;
 if(pthread_mutex_init(&runtime->mutex,NULL)){free(runtime);return -5;}
 if(pthread_cond_init(&runtime->condition,NULL)){pthread_mutex_destroy(&runtime->mutex);free(runtime);return -5;}
 runtime->phase=PREPARING;runtime->first_thread=pthread_self();
 if((*env)->GetJavaVM(env,&runtime->vm)!=JNI_OK){pthread_cond_destroy(&runtime->condition);pthread_mutex_destroy(&runtime->mutex);free(runtime);return -5;}
 KljBionicRuntime *expected=NULL;
 if(!atomic_compare_exchange_strong_explicit(&current_runtime,&expected,runtime,memory_order_acq_rel,memory_order_acquire)){pthread_cond_destroy(&runtime->condition);pthread_mutex_destroy(&runtime->mutex);free(runtime);return -16;}
 /* From publication onward runtime/code is retained even on failure: the
  * original process may already hold host-module function pointers. */
 *output=runtime;int64_t status=0;runtime->abi=dlopen(options->abi,RTLD_NOW|RTLD_LOCAL);
 ProcessPrepare prepare=runtime->abi?(ProcessPrepare)dlsym(runtime->abi,"klj_bionic_process_prepare"):NULL;
 void *signal=dlsym(RTLD_DEFAULT,"JVM_handle_bsd_signal");
 if(!prepare||!signal)status=-2;
 if(!status){ProcessOptions process={sizeof(process),KLJ_BIONIC_RUNTIME_ABI,options->root,options->path_map,options->program,options->runtime,options->host_module,options->identity,options->envp,options->envc};status=prepare(&process,&runtime->initial_token,signal);}
 if(!status){
  pthread_mutex_lock(&runtime->mutex);while(!runtime->service_ready&&!runtime->failure)pthread_cond_wait(&runtime->condition,&runtime->mutex);status=runtime->failure;pthread_mutex_unlock(&runtime->mutex);
 }
 RuntimeThread *thread=NULL;
 if(!status){
  if(!runtime->admission||!runtime->initial_token||runtime->guest.tid!=(uint64_t)klj_thread_id())status=-5;
  else if(!(thread=calloc(1,sizeof(*thread))))status=-12;
 }
 if(!status){
  *thread=(RuntimeThread){.runtime=runtime,.token=runtime->initial_token,.environment=env,.initial=1};current_thread=thread;
  uint64_t result=0;status=runtime->admission->invoke(thread->token,runtime->guest.jni_install,(uintptr_t)klj_bionic_jni_host_bindings(),&result);
  if(!status&&result)status=(int64_t)result;
  if(!status)status=runtime->admission->invoke(thread->token,runtime->guest.jni_tables,0,&result);
  if(!status){runtime->jni=klj_bionic_jni_owner(env,(const void*)(uintptr_t)result,prepare_attachment,runtime);if(!runtime->jni)status=-5;}
  if(!status){runtime->observer=(KljJniThreadObserver){.javaEnd=retire_java_thread,.owner=runtime};if(!klj_jni_thread_observe(&runtime->observer))status=-5;}
 }
 pthread_mutex_lock(&runtime->mutex);if(status){fail_locked(runtime,status);runtime->phase=FAILED;}else runtime->phase=READY;pthread_mutex_unlock(&runtime->mutex);return status;
}
const KljBionicGuest *klj_bionic_runtime_guest(KljBionicRuntime *runtime){return runtime?&runtime->guest:NULL;}
KljBionicJniOwner *klj_bionic_runtime_jni_owner(KljBionicRuntime *runtime){return runtime?runtime->jni:NULL;}
int64_t klj_bionic_runtime_packet(KljBionicRuntime *runtime,JNIEnv *env,KljCallPacket *packet){
 if(!runtime||!packet)return -22;uint64_t result=0;int64_t status=klj_bionic_runtime_invoke(runtime,env,runtime->guest.packet,(uintptr_t)packet,&result);return status?status:(int64_t)result;
}
int64_t klj_bionic_runtime_native_packet(KljBionicRuntime *runtime,KljCallPacket *packet){
 if(!runtime||!packet)return -22;uint64_t result=0;
 int64_t status=invoke(runtime,NULL,runtime->guest.packet,(uintptr_t)packet,&result,0,1);
 return status?status:(int64_t)result;
}
int64_t klj_bionic_runtime_open(KljBionicRuntime *runtime,JNIEnv *env,KljBionicOpen *request){
 if(!runtime||!request)return -22;uint64_t result=0;int64_t status=klj_bionic_runtime_invoke(runtime,env,runtime->guest.open_prepared,(uintptr_t)request,&result);return status?status:(int64_t)result;
}
int64_t klj_bionic_runtime_symbol(KljBionicRuntime *runtime,JNIEnv *env,KljBionicSymbol *request){
 if(!runtime||!request)return -22;uint64_t result=0;int64_t status=klj_bionic_runtime_invoke(runtime,env,runtime->guest.symbol,(uintptr_t)request,&result);return status?status:(int64_t)result;
}
int64_t klj_bionic_runtime_close_image(KljBionicRuntime *runtime,JNIEnv *env,KljBionicCloseImage *request){
 if(!runtime||!request)return -22;uint64_t result=0;int64_t status=klj_bionic_runtime_invoke(runtime,env,runtime->guest.close_image,(uintptr_t)request,&result);return status?status:(int64_t)result;
}
int64_t klj_bionic_runtime_prepared_callback(KljBionicRuntime *runtime,KljBionicPreparedCallback callback,void *owner){
 if(!runtime||!callback)return -22;pthread_mutex_lock(&runtime->mutex);
 int64_t status=runtime->prepared&&(runtime->prepared!=callback||runtime->prepared_owner!=owner)?-16:0;
 if(!status){runtime->prepared=callback;runtime->prepared_owner=owner;}pthread_mutex_unlock(&runtime->mutex);return status;
}
int64_t klj_bionic_runtime_status(KljBionicRuntime *runtime){
 if(!runtime)return -22;pthread_mutex_lock(&runtime->mutex);int64_t status=runtime->failure?runtime->failure:(runtime->phase==READY?0:runtime->phase==CLOSED?-108:-16);pthread_mutex_unlock(&runtime->mutex);return status;
}
int64_t klj_bionic_runtime_try_close(KljBionicRuntime *runtime,JNIEnv *env){
 if(!runtime||!env)return -22;if(!pthread_equal(pthread_self(),runtime->first_thread))return -1;
 pthread_mutex_lock(&runtime->mutex);
 if(runtime->failure){int64_t status=runtime->failure;pthread_mutex_unlock(&runtime->mutex);return status;}
 if(runtime->phase==CLOSED){pthread_mutex_unlock(&runtime->mutex);return 0;}
 if(runtime->phase!=READY&&runtime->phase!=DRAINING){pthread_mutex_unlock(&runtime->mutex);return -16;}
 if(runtime->ordinary_threads||runtime->active_entries||runtime->head){pthread_mutex_unlock(&runtime->mutex);return -16;}
 int stopping=runtime->phase==READY;runtime->phase=DRAINING;pthread_mutex_unlock(&runtime->mutex);
 if(stopping){Request request={.operation=REQUEST_STOP};int64_t status=enqueue(runtime,&request);if(status)return status;}
 /* Original Bionic has no tryjoin API. Its canonical identity retains the
  * exact Mach thread through POSIX destruction, so absence proves the original
  * join can no longer wait for guest execution or host-thread retirement. */
 thread_t coordinator=klj_thread_acquire((int)runtime->guest.coordinator_tid);
 if(coordinator){mach_port_deallocate(mach_task_self(),coordinator);return -16;}
 if(errno!=ESRCH)return -5;
 uint64_t result=0;int64_t status=invoke(runtime,env,runtime->guest.try_join_coordinator,0,&result,1,0);if(!status)status=(int64_t)result;
 if(!status){pthread_mutex_lock(&runtime->mutex);runtime->phase=CLOSED;pthread_mutex_unlock(&runtime->mutex);}return status;
}
