#define _GNU_SOURCE
#include "admission.h"
#include "call-packet.h"
#include <assert.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <link.h>
#include "jni-carrier.h"
static State state;
State *fixture_state(void){return &state;}
int64_t fixture_host(uint32_t function,void *args,size_t length){
 register uint64_t x0 __asm__("x0")=0x4d4c,x1 __asm__("x1")=function,x2 __asm__("x2")=(uintptr_t)args,x3 __asm__("x3")=length,x8 __asm__("x8")=0x48430000;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","x16","x17","x30","memory","cc");return (int64_t)x0;
}
static uint64_t yield(unsigned reason){register uint64_t x0 __asm__("x0")=reason,x8 __asm__("x8")=KLJ_BIONIC_YIELD_NR;__asm__ volatile("svc #0":"+r"(x0):"r"(x8):"memory","cc");return x0;}
static void *admitted_start(void *arg){(void)arg;uint64_t raw=yield(KLJ_BIONIC_READY);for(;;){KljBionicAdmissionCall *call=(void*)(uintptr_t)raw;assert(call);if(!call->function)return 0;uint64_t(*function)(uint64_t)=(void*)(uintptr_t)call->function;call->result=function(call->argument);raw=yield(KLJ_BIONIC_DONE);}}
static uint64_t prepare(uint64_t raw){Prepared *request=(void*)(uintptr_t)raw;pthread_t thread=0;request->error=pthread_create(&thread,0,admitted_start,(void*)(uintptr_t)request->token);request->thread=(uintptr_t)thread;return request->error;}
static uint64_t join(uint64_t raw){Prepared *request=(void*)(uintptr_t)raw;void *result=(void*)1;int error=pthread_join((pthread_t)(uintptr_t)request->thread,&result);assert(!error&&!result);return 0;}
static uint64_t nested(uint64_t raw){assert(state.constructor_pthread==(uintptr_t)pthread_self());assert(state.errno_address==(uintptr_t)__errno());assert(errno==219);state.nested++;return raw+5;}
static void *library;
static uint64_t resolve(uint64_t raw){Lookup *lookup=(void*)(uintptr_t)raw;if(!library){library=dlopen("/data/local/tmp/bionic-admission/libadmitted.so",RTLD_NOW|RTLD_LOCAL);assert(library);}lookup->result=(uintptr_t)dlsym(library,lookup->name);assert(lookup->result);return lookup->result;}
static Mapped mapped;
static int image_info(struct dl_phdr_info *info,size_t size,void *raw){(void)size;uintptr_t target=(uintptr_t)raw;for(size_t i=0;i<info->dlpi_phnum;i++){const ElfW(Phdr)*p=&info->dlpi_phdr[i];if(p->p_type==PT_LOAD&&target>=info->dlpi_addr+p->p_vaddr&&target<info->dlpi_addr+p->p_vaddr+p->p_memsz){mapped=(Mapped){target==(uintptr_t)klj_call_packet?dlopen(NULL,RTLD_NOW):library,info->dlpi_addr,info->dlpi_phdr,info->dlpi_phnum};return 1;}}return 0;}
static uint64_t metadata(uint64_t target){assert(dl_iterate_phdr(image_info,(void*)(uintptr_t)target)==1&&mapped.identity);return(uintptr_t)&mapped;}
static void *coordinator(void *raw){assert(!fixture_host(10,raw,sizeof(Bootstrap)));return 0;}
static uint64_t initial(uint64_t raw){(void)raw;assert(getpid()==gettid());return((uint64_t)getpid()<<32)|(uint32_t)gettid();}
int main(void){setvbuf(stdout,0,_IONBF,0);Bootstrap bootstrap={(uintptr_t)prepare,(uintptr_t)join,0,(uintptr_t)nested,(uintptr_t)&state,(uintptr_t)initial,(uintptr_t)resolve,(uintptr_t)klj_call_packet,(uintptr_t)klj_bionic_jni_guest_install,(uintptr_t)klj_bionic_jni_guest_tables,(uintptr_t)metadata};pthread_t worker;assert(!pthread_create(&worker,0,coordinator,&bootstrap));uint64_t raw=yield(KLJ_BIONIC_READY);for(;;){KljBionicAdmissionCall *call=(void*)(uintptr_t)raw;assert(call&&call->function);uint64_t(*function)(uint64_t)=(void*)(uintptr_t)call->function;call->result=function(call->argument);raw=yield(KLJ_BIONIC_DONE);}}
