#include "admission.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
extern void *__dso_handle;
extern int __cxa_thread_atexit_impl(void(*)(void*),void*,void*);
static void key_end(void *raw){State*s=raw;assert(s->constructor_pthread==(uintptr_t)pthread_self());s->key_destructors++;}
static void cxa_end(void *raw){State*s=raw;assert(s->constructor_pthread==(uintptr_t)pthread_self());s->cxa_destructors++;}
__attribute__((constructor))static void initialize(void){State*s=fixture_state();assert(!s->constructors);s->constructors++;s->constructor_tid=gettid();s->constructor_pthread=(uintptr_t)pthread_self();s->errno_address=(uintptr_t)__errno();__asm__ volatile("mrs %0,tpidr_el0":"=r"(s->constructor_tp));assert(s->constructor_tp);pthread_key_t key;assert(!pthread_key_create(&key,key_end)&&!pthread_setspecific(key,s));assert(!__cxa_thread_atexit_impl(cxa_end,s,&__dso_handle));assert(!fixture_host(42,s,sizeof *s));}
JNIEXPORT jlong JNICALL Java_BionicAdmissionProbe_roundTrip(JNIEnv *env,jclass type,jint a,jlong b,jfloat c,jdouble d,jint e,jint f,jint g,jint h,jint i,jint j){State*s=fixture_state();assert(a==11&&b==22&&c==1.5f&&d==2.5&&e==33&&f==44&&g==55&&h==66&&i==77&&j==88);assert(s->constructor_tid==(uint64_t)gettid()&&s->constructor_pthread==(uintptr_t)pthread_self()&&s->errno_address==(uintptr_t)__errno());if(s->calls)assert(errno==219);errno=219;s->calls++;JavaCallback callback={env,type,399,0};assert(!fixture_host(43,&callback,sizeof callback));assert(errno==219);return callback.result;}
