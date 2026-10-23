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
static State *entered(void){State*s=fixture_state();assert(s->constructor_tid==(uint64_t)gettid()&&s->constructor_pthread==(uintptr_t)pthread_self()&&s->errno_address==(uintptr_t)__errno());return s;}
JNIEXPORT jlong JNICALL Java_BionicProcessProbe_roundTrip(JNIEnv *env,jclass type,jint a,jlong b,jfloat c,jdouble d,jint e,jint f,jint g,jint h,jint i,jint j){State*s=entered();assert(a==11&&b==22&&c==1.5f&&d==2.5&&e==33&&f==44&&g==55&&h==66&&i==77&&j==88);if(s->calls)assert(errno==219);errno=219;s->calls++;JavaCallback callback={env,type,399,0};assert(!fixture_host(43,&callback,sizeof callback));assert(errno==219);return callback.result;}
JNIEXPORT jlong JNICALL Java_BionicProcessProbe_nested(JNIEnv *env,jclass type,jlong value){(void)env;(void)type;State*s=entered();assert(errno==219);s->nested++;return value+5;}
JNIEXPORT jlong JNICALL Java_BionicProcessProbe_narrow(JNIEnv *env,jclass type,jboolean z,jbyte b,jchar c,jshort s,jint i,jlong l,jint a,jint d,jint e,jint f){(void)env;(void)type;entered();assert(z==1&&b==-7&&c==65530&&s==-30000&&i==0x12345678&&l==INT64_C(0x1122334455667788)&&a==-11&&d==22&&e==-33&&f==44);return (jlong)UINT64_C(0x8877665544332211);}
JNIEXPORT jdouble JNICALL Java_BionicProcessProbe_floating(JNIEnv *env,jclass type,jfloat a0,jfloat a1,jfloat a2,jfloat a3,jfloat a4,jfloat a5,jfloat a6,jfloat a7,jfloat a8,jdouble d0,jdouble d1,jdouble d2,jdouble d3,jdouble d4,jdouble d5,jdouble d6,jdouble d7,jdouble d8){(void)env;(void)type;entered();assert(a0==.25f&&a1==1.25f&&a2==2.25f&&a3==3.25f&&a4==4.25f&&a5==5.25f&&a6==6.25f&&a7==7.25f&&a8==8.25f);assert(d0==1000.5&&d1==1001.5&&d2==1002.5&&d3==1003.5&&d4==1004.5&&d5==1005.5&&d6==1006.5&&d7==1007.5&&d8==1008.5);return a8+d8;}
JNIEXPORT jobject JNICALL Java_BionicProcessProbe_object(JNIEnv *env,jclass type,jobject value){(void)env;(void)type;entered();return value;}
JNIEXPORT jboolean JNICALL Java_BionicProcessProbe_booleanValue(JNIEnv *e,jclass c,jboolean v){(void)e;(void)c;entered();return !v;}
JNIEXPORT jbyte JNICALL Java_BionicProcessProbe_byteValue(JNIEnv *e,jclass c,jbyte v){(void)e;(void)c;entered();return v-1;}
JNIEXPORT jchar JNICALL Java_BionicProcessProbe_charValue(JNIEnv *e,jclass c,jchar v){(void)e;(void)c;entered();return v+1;}
JNIEXPORT jshort JNICALL Java_BionicProcessProbe_shortValue(JNIEnv *e,jclass c,jshort v){(void)e;(void)c;entered();return v+1;}
JNIEXPORT jint JNICALL Java_BionicProcessProbe_intValue(JNIEnv *e,jclass c,jint v){(void)e;(void)c;entered();return v^0x11223344;}
JNIEXPORT jlong JNICALL Java_BionicProcessProbe_longValue(JNIEnv *e,jclass c,jlong v){(void)e;(void)c;entered();return v^INT64_C(0x1122334455667788);}
JNIEXPORT jfloat JNICALL Java_BionicProcessProbe_floatValue(JNIEnv *e,jclass c,jfloat v){(void)e;(void)c;entered();return v*2;}
JNIEXPORT void JNICALL Java_BionicProcessProbe_voidValue(JNIEnv *e,jclass c,jint v){(void)e;(void)c;State*s=entered();assert(v==73);s->calls++;}
JNIEXPORT void JNICALL Java_BionicProcessProbe_throwing(JNIEnv *e,jclass c){entered();JavaCallback call={e,c,0,0};assert(!fixture_host(44,&call,sizeof call));}
typedef struct{uint64_t a,b,c;} Large;
typedef struct{uint64_t a,b;} Pair;
typedef struct{float a,b,c,d;} Hfa;
typedef uint64_t Vector __attribute__((vector_size(16)));
Large fixture_large(uint64_t a,uint64_t b,uint64_t c){entered();return (Large){a+1,b+2,c+3};}
Pair fixture_pair(uint64_t a,uint64_t b){entered();return (Pair){a+3,b+4};}
Hfa fixture_hfa(float v){entered();return(Hfa){v,v+1,v+2,v+3};}
Vector fixture_vector(Vector v){entered();return v+(Vector){3,5};}
