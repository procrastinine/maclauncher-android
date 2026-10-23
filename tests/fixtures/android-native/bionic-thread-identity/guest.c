#define _GNU_SOURCE
#include "admission.h"
#include <assert.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
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
static uint64_t dispatch(uint64_t raw){Typed *call=(void*)(uintptr_t)raw;
 if(!call->function){void *library=dlopen("/data/local/tmp/bionic-admission/libadmitted.so",RTLD_NOW|RTLD_LOCAL);assert(library);call->function=(uintptr_t)dlsym(library,"Java_BionicAdmissionProbe_roundTrip");assert(call->function);}
 const jvalue*v=call->values;jlong(*function)(JNIEnv*,jclass,jint,jlong,jfloat,jdouble,jint,jint,jint,jint,jint,jint)=(void*)(uintptr_t)call->function;
 call->result=function(call->env,call->type,v[0].i,v[1].j,v[2].f,v[3].d,v[4].i,v[5].i,v[6].i,v[7].i,v[8].i,v[9].i);return call->function;
}
static void *coordinator(void *raw){assert(!fixture_host(10,raw,sizeof(Bootstrap)));return 0;}
static void *jvm(void *raw){assert(!fixture_host(11,raw,sizeof(Bootstrap)));return 0;}
int main(void){setvbuf(stdout,0,_IONBF,0);Bootstrap bootstrap={(uintptr_t)prepare,(uintptr_t)join,(uintptr_t)dispatch,(uintptr_t)nested,(uintptr_t)&state};pthread_t worker;assert(!pthread_create(&worker,0,coordinator,&bootstrap));pthread_t vm_thread;assert(!pthread_create(&vm_thread,0,jvm,&bootstrap));assert(!pthread_join(vm_thread,0));assert(!pthread_join(worker,0));assert(state.constructors==1&&state.key_destructors==1&&state.cxa_destructors==1&&state.calls==1&&state.nested==1&&state.callbacks==1&&state.ended==1);puts("PASS canonical shared JavaThread TID reused before Bionic publication collision rejected lease retained through ThreadEnd and late destructor retired after Mach death");return 0;}
