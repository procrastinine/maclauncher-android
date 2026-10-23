#define _GNU_SOURCE
#include "runtime-protocol.h"
#include "thread-admission.h"
#include "call-packet.h"
#include "jni-carrier.h"
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

typedef void *(*PreparedOpen)(const char*,int,const void*,const void*,const MaclauncherDlopenPrepareV1*);
static PreparedOpen prepared_open;
static int linker_event(void *unused,uint32_t phase,const void *caller,
 const MaclauncherDlopenGraphV1 *graph,void *handle,uintptr_t bias,char *error,size_t capacity);
static pthread_t coordinator_thread;
static KljBionicGuest bootstrap;
static int64_t host_call(uint32_t function,void *argument,size_t length){
 register uint64_t x0 __asm__("x0")=0x4d4c,x1 __asm__("x1")=function,x2 __asm__("x2")=(uintptr_t)argument,x3 __asm__("x3")=length,x8 __asm__("x8")=0x48430000;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","x16","x17","x30","memory","cc");return(int64_t)x0;
}
static uint64_t yield(unsigned reason){register uint64_t x0 __asm__("x0")=reason,x8 __asm__("x8")=KLJ_BIONIC_YIELD_NR;__asm__ volatile("svc #0":"+r"(x0):"r"(x8):"memory","cc");return x0;}
static void *admitted(void *unused){
 (void)unused;uint64_t raw=yield(KLJ_BIONIC_READY);
 for(;;){KljBionicAdmissionCall *call=(void*)(uintptr_t)raw;if(!call||!call->function)return NULL;call->result=((uint64_t(*)(uint64_t))(uintptr_t)call->function)(call->argument);raw=yield(KLJ_BIONIC_DONE);}
}
static uint64_t prepare(uint64_t raw){
 KljBionicThreadRequest *request=(void*)(uintptr_t)raw;if(!request)return(uint64_t)-EINVAL;
 pthread_t thread=0;request->error=pthread_create(&thread,NULL,admitted,NULL);request->thread=(uintptr_t)thread;return(uint64_t)-(int64_t)request->error;
}
static uint64_t join(uint64_t raw){
 KljBionicThreadRequest *request=(void*)(uintptr_t)raw;if(!request||!request->thread)return(uint64_t)-EINVAL;
 void *result=NULL;int error=pthread_join((pthread_t)(uintptr_t)request->thread,&result);return error?(uint64_t)-(int64_t)error:result?(uint64_t)-EIO:0;
}
/* Bionic supplies pthread_join, not pthread_tryjoin_np. The host only invokes
 * this after its retained canonical thread identity observes kernel death. */
static uint64_t join_coordinator(uint64_t unused){(void)unused;void *result=NULL;int error=pthread_join(coordinator_thread,&result);return error?(uint64_t)-(int64_t)error:result?(uint64_t)-EIO:0;}
static int prepared(void *context,const MaclauncherDlopenGraphV1 *graph,char *error,size_t capacity){
 KljBionicOpen *request=context;KljBionicPreparedEvent event={sizeof(event),KLJ_BIONIC_RUNTIME_ABI,request->context,graph,error,capacity};
 return host_call(KLJ_BIONIC_PREPARED_HOSTCALL,&event,sizeof(event))==1;
}
static void copy_error(char *output,size_t capacity,const char *error){if(output&&capacity)snprintf(output,capacity,"%s",error?error:"Android native loader operation failed");}
/* Android zygote normally selects this original per-process loader policy
 * before application code. The host supplies the installed package policy;
 * original 16KiB eligibility, mappings, relocation and RELRO checks remain. */
uint64_t klj_bionic_page_compat(uint64_t enabled){
 if(enabled>1)return(uint64_t)-EINVAL;
 typedef void(*SetCompat)(bool);int saved=errno;
 SetCompat set=(SetCompat)dlsym(RTLD_DEFAULT,"__loader_android_set_16kb_appcompat_mode");
 if(!set){errno=saved;return(uint64_t)-ENOSYS;}set(enabled!=0);errno=saved;return 0;
}
/* JNI installs the authenticated process package's already mounted library
 * directory after the platform bootstrap has linked and before app code runs.
 * Name-only dlopen and DT_NEEDED then use Bionic's original caller namespace,
 * search order and ownership observer. Inherited host search paths stay absent. */
uint64_t klj_bionic_application_libraries(uint64_t raw){
 const char *path=(const char*)(uintptr_t)raw;
 if(!path||path[0]!='/'||strpbrk(path,":\r\n\t"))return(uint64_t)-EINVAL;
 typedef void(*UpdatePath)(const char*);int saved=errno;
 UpdatePath update=(UpdatePath)dlsym(RTLD_DEFAULT,"__loader_android_update_LD_LIBRARY_PATH");
 if(!update){errno=saved;return(uint64_t)-ENOSYS;}
 /* The original void loader API resolves and checks the directory itself.
  * A search-path update carries no filesystem authority. */
 update(path);errno=saved;return 0;
}
/* Android normally sends native logs to logd. This standalone process uses
 * the original liblog stderr sink, matching its existing launcher log pipe.
 * Both public liblog APIs and filtering stay upstream-owned. Configure once,
 * before JNI_OnLoad; later application logger changes remain authoritative. */
static void native_log_output(void *handle){
 static atomic_bool installed;
 if(atomic_load_explicit(&installed,memory_order_acquire))return;
 typedef void (*SetLogger)(__android_logger_function);
 int saved=errno;
 SetLogger set=(SetLogger)dlsym(handle,"__android_log_set_logger");
 __android_logger_function output=(__android_logger_function)dlsym(handle,"__android_log_stderr_logger");
 if(set&&output&&!atomic_exchange_explicit(&installed,true,memory_order_acq_rel))set(output);
 errno=saved;
}
static int linker_event(void *unused,uint32_t phase,const void *caller,
 const MaclauncherDlopenGraphV1 *graph,void *handle,uintptr_t bias,char *error,size_t capacity){
 (void)unused;KljBionicLinkerEvent event={sizeof(event),KLJ_BIONIC_RUNTIME_ABI,phase,0,caller,graph,handle,bias,error,capacity};
 return host_call(KLJ_BIONIC_LINKER_HOSTCALL,&event,sizeof(event))==1;
}
static uint64_t open_prepared(uint64_t raw){
 KljBionicOpen *request=(void*)(uintptr_t)raw;
 if(!request||request->size!=sizeof(*request)||request->version!=KLJ_BIONIC_RUNTIME_ABI||!request->filename)return(uint64_t)-EINVAL;
 request->handle=NULL;if(request->error&&request->error_capacity)request->error[0]=0;
 if(!prepared_open){copy_error(request->error,request->error_capacity,"Android prepared native loading is unavailable");return(uint64_t)-ENOSYS;}
 {
  int (*install)(const MaclauncherDlopenObserverV1*)=dlsym(RTLD_DEFAULT,"__loader_maclauncher_install_dlopen_observer_v1");
  if(!install){copy_error(request->error,request->error_capacity,"Android native dlopen ownership is unavailable");return(uint64_t)-ENOSYS;}
  const MaclauncherDlopenObserverV1 observer={sizeof(observer),MACLAUNCHER_DLOPEN_OBSERVER_ABI,NULL,linker_event};
  int status=install(&observer);if(status)return(uint64_t)status;
 }
 MaclauncherDlopenPrepareV1 callback={sizeof(callback),MACLAUNCHER_DLOPEN_PREPARE_ABI,request,prepared};
 request->handle=prepared_open(request->filename,request->flags,NULL,request->caller,&callback);
 if(!request->handle){copy_error(request->error,request->error_capacity,dlerror());return(uint64_t)-ENOENT;}native_log_output(request->handle);return 0;
}
static uint64_t symbol(uint64_t raw){
 KljBionicSymbol *request=(void*)(uintptr_t)raw;
 if(!request||request->size!=sizeof(*request)||request->version!=KLJ_BIONIC_RUNTIME_ABI||!request->name)return(uint64_t)-EINVAL;
 if(request->error&&request->error_capacity)request->error[0]=0;dlerror();request->symbol=dlsym(request->handle,request->name);const char *error=dlerror();
 if(error){copy_error(request->error,request->error_capacity,error);return(uint64_t)-ENOENT;}return 0;
}
static uint64_t close_image(uint64_t raw){
 KljBionicCloseImage *request=(void*)(uintptr_t)raw;
 if(!request||request->size!=sizeof(*request)||request->version!=KLJ_BIONIC_RUNTIME_ABI||!request->handle)return(uint64_t)-EINVAL;
 if(request->error&&request->error_capacity)request->error[0]=0;
 if(dlclose(request->handle)){copy_error(request->error,request->error_capacity,dlerror());return(uint64_t)-EINVAL;}return 0;
}
static int helper_image(struct dl_phdr_info *info,size_t size,void *unused){
 (void)size;(void)unused;uintptr_t address=(uintptr_t)klj_call_packet,min=UINTPTR_MAX,max=0;int contains=0;
 for(size_t i=0;i<info->dlpi_phnum;i++){
  const ElfW(Phdr)*p=&info->dlpi_phdr[i];if(p->p_type!=PT_LOAD)continue;
  if(p->p_vaddr>UINTPTR_MAX-info->dlpi_addr)continue;uintptr_t begin=info->dlpi_addr+p->p_vaddr;
  if(p->p_memsz>UINTPTR_MAX-begin)continue;uintptr_t end=begin+p->p_memsz;
  if(address>=begin&&address<end)contains=1;if(begin<min)min=begin;if(end>max)max=end;
 }
 if(!contains||max<=min)return 0;
 struct stat st={0};if(info->dlpi_name&&info->dlpi_name[0])stat(info->dlpi_name,&st);
 bootstrap.helper=(MaclauncherDlopenImageV1){.size=sizeof(bootstrap.helper),.flags=MACLAUNCHER_DLOPEN_IMAGE_ROOT|MACLAUNCHER_DLOPEN_IMAGE_CONSTRUCTED,.handle=dlopen(NULL,RTLD_NOW),.mapping_base=min,.mapping_size=max-min,.load_bias=info->dlpi_addr,.name=info->dlpi_name,.phdr=info->dlpi_phdr,.phnum=info->dlpi_phnum,.device=st.st_dev,.inode=st.st_ino};return 1;
}
static void *coordinator(void *unused){
 (void)unused;bootstrap.coordinator_tid=gettid();return(void*)(uintptr_t)host_call(KLJ_BIONIC_BOOTSTRAP_HOSTCALL,&bootstrap,sizeof(bootstrap));
}
int main(void){
 prepared_open=(PreparedOpen)dlsym(RTLD_DEFAULT,"__loader_maclauncher_dlopen_prepared_v1");
 bootstrap=(KljBionicGuest){.size=sizeof(bootstrap),.version=KLJ_BIONIC_RUNTIME_ABI,.prepare=(uintptr_t)prepare,.join=(uintptr_t)join,.packet=(uintptr_t)klj_call_packet,.jni_install=(uintptr_t)klj_bionic_jni_guest_install,.jni_tables=(uintptr_t)klj_bionic_jni_guest_tables,.open_prepared=(uintptr_t)open_prepared,.symbol=(uintptr_t)symbol,.close_image=(uintptr_t)close_image,.try_join_coordinator=(uintptr_t)join_coordinator,.pid=getpid(),.tid=gettid(),.prepared_entry=(uintptr_t)prepared_open};
 if(dl_iterate_phdr(helper_image,NULL)!=1||!bootstrap.helper.handle)return 127;
 if(pthread_create(&coordinator_thread,NULL,coordinator,NULL))return 126;
 (void)admitted(NULL);return 0;
}
