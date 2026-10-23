#include "jvm-bionic.h"
#include "jvm-bionic-jni.h"
#include <stdlib.h>
#include <pthread.h>
#include "bionic-provider.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
static _Thread_local KljBionicProvider active_provider;
static _Thread_local uint64_t active_dispatch,active_function;
static _Thread_local JNIEnv *active_environment;
static jmethodID callback_method;
static jlong JNICALL native_round_trip(JNIEnv *env,jclass type,jint a,jlong b,jfloat c,jdouble d,jint e,jint f,jint g,jint h,jint i,jint j){
 assert(env==active_environment&&active_provider.enter&&active_dispatch&&active_function);
 FixtureJniCall call={.function=active_function,.env=env,.type=type,.values={{.i=a},{.j=b},{.f=c},{.d=d},{.i=e},{.i=f},{.i=g},{.i=h},{.i=i},{.i=j}}};
 assert(!active_provider.enter(active_dispatch,(uintptr_t)&call));return call.result;
}
static int64_t run_vm(FixtureJvm *request,KljBionicGuestCallback callback){
 const char *library=getenv("KLJ_BIONIC_TEST_JVM"),*classes=getenv("KLJ_BIONIC_TEST_CLASSES");assert(library&&classes);
 void *jvm=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!jvm)fprintf(stderr,"libjvm: %s\n",dlerror());assert(jvm);
 jint (*create)(JavaVM**,void**,void*)=dlsym(jvm,"JNI_CreateJavaVM");assert(create);
 char classpath[4096];assert(snprintf(classpath,sizeof classpath,"-Djava.class.path=%s",classes)<(int)sizeof classpath);
 JavaVMOption options[]={{classpath,0},{"-Xcheck:jni",0},{"-Xmx64m",0},{"-XX:+UseSerialGC",0},{"-XX:ActiveProcessorCount=2",0},{"-Xss256k",0},{"-Xrs",0}};
 JavaVMInitArgs arguments={.version=JNI_VERSION_1_8,.nOptions=sizeof options/sizeof options[0],.options=options,.ignoreUnrecognized=JNI_FALSE};
 JavaVM *vm=0;JNIEnv *env=0;jint status=create(&vm,(void**)&env,&arguments);fprintf(stderr,"JNI_CreateJavaVM=%d\n",status);assert(status==JNI_OK&&vm&&env);
 JNIEnv *same=0;assert((*vm)->GetEnv(vm,(void**)&same,JNI_VERSION_1_8)==JNI_OK&&same==env);
 active_provider=(KljBionicProvider){callback,request->dispatcher};active_dispatch=request->typed_dispatcher;active_environment=env;
 char error[512];void *guest=0,*entry=0;int opened=klj_bionic_open(&active_provider,request->library,KLJ_BIONIC_RTLD_NOW|KLJ_BIONIC_RTLD_LOCAL,&guest,error,sizeof error);if(!opened)fprintf(stderr,"Bionic JNI dlopen: %s\n",error);assert(opened);
 assert(klj_bionic_symbol(&active_provider,guest,"Java_BionicProviderProbe_roundTrip",&entry,error,sizeof error));active_function=(uintptr_t)entry;
 jclass type=(*env)->FindClass(env,"BionicProviderProbe");assert(type&&!(*env)->ExceptionCheck(env));
 callback_method=(*env)->GetStaticMethodID(env,type,"callback","(J)J");assert(callback_method);
 JNINativeMethod method={"roundTrip","(IJFDIIIIII)J",(void*)native_round_trip};assert((*env)->RegisterNatives(env,type,&method,1)==JNI_OK);
 jmethodID run=(*env)->GetStaticMethodID(env,type,"run","()V");assert(run);(*env)->CallStaticVoidMethodA(env,type,run,0);
 jthrowable failure=(*env)->ExceptionOccurred(env);if(failure)(*env)->ExceptionDescribe(env);assert(!failure);
 assert((*env)->UnregisterNatives(env,type)==JNI_OK);(*env)->DeleteLocalRef(env,type);assert(klj_bionic_close(&active_provider,guest,error,sizeof error));
 assert((*vm)->DestroyJavaVM(vm)==JNI_OK);active_provider=(KljBionicProvider){0};active_environment=0;return 0;
}
int64_t maclauncher_art_host_call(uint32_t function,uint64_t args,uint64_t length,KljBionicGuestCallback callback){
 if(function==2)return 0xabc;
 if(function==3){assert(length==sizeof(FixtureJvm));return run_vm((void*)(uintptr_t)args,callback);}
 if(function==4){assert(length==sizeof(FixtureJavaCallback));FixtureJavaCallback *request=(void*)(uintptr_t)args;assert(request->env==active_environment&&callback_method);request->result=(*request->env)->CallStaticLongMethodA(request->env,request->type,callback_method,&request->argument);assert(!(*request->env)->ExceptionCheck(request->env));return 0;}
 if(function!=1||length!=sizeof(FixtureRequest))return -22;
 FixtureRequest *request=(void*)(uintptr_t)args;KljBionicProvider provider={callback,request->dispatcher};
 char error[512];void *libc=0,*getpid=0,*errno_address=0,*mmap_address=0,*munmap_address=0,*self=0;
 assert(klj_bionic_open(&provider,"libc.so",KLJ_BIONIC_RTLD_NOW|KLJ_BIONIC_RTLD_LOCAL,&libc,error,sizeof error)&&libc);
 assert(klj_bionic_symbol(&provider,libc,"getpid",&getpid,error,sizeof error)&&getpid==(void*)(uintptr_t)request->expected_getpid);
 assert(klj_bionic_symbol(&provider,libc,"__errno",&errno_address,error,sizeof error)&&errno_address==(void*)(uintptr_t)request->expected_errno);
 assert(klj_bionic_symbol(&provider,libc,"mmap",&mmap_address,error,sizeof error));
 assert(klj_bionic_symbol(&provider,libc,"munmap",&munmap_address,error,sizeof error));
 assert(klj_bionic_symbol(&provider,libc,"pthread_self",&self,error,sizeof error));
 void *missing=(void*)1;assert(!klj_bionic_symbol(&provider,libc,"maclauncher_absent_symbol",&missing,error,sizeof error)&&!missing&&strstr(error,"maclauncher_absent_symbol"));
 FixtureCall call={(uintptr_t)getpid,(uintptr_t)errno_address,(uintptr_t)mmap_address,(uintptr_t)munmap_address,(uintptr_t)self,1,0};
 assert(callback(request->check,(uintptr_t)&call)==0x12345678&&call.checks==6);
 assert(klj_bionic_close(&provider,libc,error,sizeof error));
 request->checks=8;return 0;
}
