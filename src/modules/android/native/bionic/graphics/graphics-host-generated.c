#include "graphics-arguments.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
extern int kl_errno_to_linux(int),kl_errno_from_linux(int);
static KljBionicGraphicsOwner owner;static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;static _Atomic int ready;
typedef struct {JNIEnv *env;uintptr_t caller;void *user;AndroidBitmap_CompressWriteFunc callback;} Compression;
static bool write_compressed(void *raw,const void *data,size_t size){
 Compression *c=raw;int saved=errno;KljCallPacket packet={.size=sizeof packet,.version=KLJ_CALL_PACKET_VERSION,.target=(uintptr_t)c->callback,.x={(uintptr_t)c->user,(uintptr_t)data,size}};
 int64_t status=owner.invoke(owner.context,c->env,c->caller,&packet);errno=saved;
 return !status&&!packet.status&&(!c->env||!(*c->env)->ExceptionCheck(c->env))&&(uint8_t)packet.result_x[0]!=0;
}
static int64_t unavailable(const char *name,int marshalling){fprintf(stderr,"Android NDK operation unavailable: %s (%s)\n",name,marshalling?"descriptor ownership bridge":"native provider");return -38;}
static void (*target_0)(AHardwareBuffer *);
static int64_t invoke_0(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_0 *a=(void*)(uintptr_t)packet->arguments;if(!target_0)return unavailable("AHardwareBuffer_acquire",0);target_0(a->a0);return 0;}
static int (*target_1)(const AHardwareBuffer_Desc *,AHardwareBuffer * _Nullable *);
static int64_t invoke_1(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_1 *a=(void*)(uintptr_t)packet->arguments;if(!target_1)return unavailable("AHardwareBuffer_allocate",0);a->result=target_1(a->a0,a->a1);return 0;}
static void (*target_2)(const AHardwareBuffer *,AHardwareBuffer_Desc *);
static int64_t invoke_2(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_2 *a=(void*)(uintptr_t)packet->arguments;if(!target_2)return unavailable("AHardwareBuffer_describe",0);target_2(a->a0,a->a1);return 0;}
static AHardwareBuffer * (*target_3)(JNIEnv *,jobject);
static int64_t invoke_3(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_3 *a=(void*)(uintptr_t)packet->arguments;if(!target_3)return unavailable("AHardwareBuffer_fromHardwareBuffer",0);if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env)return -22;a->result=target_3(a->a0,a->a1);return 0;}
static int (*target_4)(const AHardwareBuffer *,uint64_t *);
static int64_t invoke_4(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_4 *a=(void*)(uintptr_t)packet->arguments;if(!target_4)return unavailable("AHardwareBuffer_getId",0);a->result=target_4(a->a0,a->a1);return 0;}
static int (*target_5)(const AHardwareBuffer_Desc *);
static int64_t invoke_5(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_5 *a=(void*)(uintptr_t)packet->arguments;if(!target_5)return unavailable("AHardwareBuffer_isSupported",0);a->result=target_5(a->a0);return 0;}
static int (*target_6)(AHardwareBuffer *,uint64_t,int32_t,const ARect *,void * _Nullable *);
static int64_t invoke_6(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_6 *a=(void*)(uintptr_t)packet->arguments;if(!target_6)return unavailable("AHardwareBuffer_lock",0);if(a->a2>=0)return unavailable("AHardwareBuffer_lock",1);a->result=target_6(a->a0,a->a1,a->a2,a->a3,a->a4);return 0;}
static int (*target_7)(AHardwareBuffer *,uint64_t,int32_t,const ARect *,void * _Nullable *,int32_t *,int32_t *);
static int64_t invoke_7(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_7 *a=(void*)(uintptr_t)packet->arguments;if(!target_7)return unavailable("AHardwareBuffer_lockAndGetInfo",0);if(a->a2>=0)return unavailable("AHardwareBuffer_lockAndGetInfo",1);a->result=target_7(a->a0,a->a1,a->a2,a->a3,a->a4,a->a5,a->a6);return 0;}
static int (*target_8)(AHardwareBuffer *,uint64_t,int32_t,const ARect *,AHardwareBuffer_Planes *);
static int64_t invoke_8(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_8 *a=(void*)(uintptr_t)packet->arguments;if(!target_8)return unavailable("AHardwareBuffer_lockPlanes",0);if(a->a2>=0)return unavailable("AHardwareBuffer_lockPlanes",1);a->result=target_8(a->a0,a->a1,a->a2,a->a3,a->a4);return 0;}
static binder_status_t (*target_9)(const AParcel *,AHardwareBuffer * _Nullable *);
static int64_t invoke_9(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_9 *a=(void*)(uintptr_t)packet->arguments;if(!target_9)return unavailable("AHardwareBuffer_readFromParcel",0);a->result=target_9(a->a0,a->a1);return 0;}
static int (*target_10)(int,AHardwareBuffer * _Nullable *);
static int64_t invoke_10(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_10 *a=(void*)(uintptr_t)packet->arguments;if(!target_10)return unavailable("AHardwareBuffer_recvHandleFromUnixSocket",0);if(a->a0>=0)return unavailable("AHardwareBuffer_recvHandleFromUnixSocket",1);a->result=target_10(a->a0,a->a1);return 0;}
static void (*target_11)(AHardwareBuffer *);
static int64_t invoke_11(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_11 *a=(void*)(uintptr_t)packet->arguments;if(!target_11)return unavailable("AHardwareBuffer_release",0);target_11(a->a0);return 0;}
static int (*target_12)(const AHardwareBuffer *,int);
static int64_t invoke_12(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_12 *a=(void*)(uintptr_t)packet->arguments;if(!target_12)return unavailable("AHardwareBuffer_sendHandleToUnixSocket",0);if(a->a1>=0)return unavailable("AHardwareBuffer_sendHandleToUnixSocket",1);a->result=target_12(a->a0,a->a1);return 0;}
static jobject (*target_13)(JNIEnv *,AHardwareBuffer *);
static int64_t invoke_13(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_13 *a=(void*)(uintptr_t)packet->arguments;if(!target_13)return unavailable("AHardwareBuffer_toHardwareBuffer",0);if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env)return -22;a->result=target_13(a->a0,a->a1);return 0;}
static int (*target_14)(AHardwareBuffer *,int32_t *);
static int64_t invoke_14(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_14 *a=(void*)(uintptr_t)packet->arguments;if(!target_14)return unavailable("AHardwareBuffer_unlock",0);if(a->a1)return unavailable("AHardwareBuffer_unlock",1);a->result=target_14(a->a0,NULL);return 0;}
static binder_status_t (*target_15)(const AHardwareBuffer *,AParcel *);
static int64_t invoke_15(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_15 *a=(void*)(uintptr_t)packet->arguments;if(!target_15)return unavailable("AHardwareBuffer_writeToParcel",0);a->result=target_15(a->a0,a->a1);return 0;}
static int (*target_16)(const AndroidBitmapInfo *,int32_t,const void *,int32_t,int32_t,void *,AndroidBitmap_CompressWriteFunc);
static int64_t invoke_16(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_16 *a=(void*)(uintptr_t)packet->arguments;if(!target_16)return unavailable("AndroidBitmap_compress",0);if(a->a6&&!owner.invoke)return unavailable("AndroidBitmap_compress callback",0);Compression c={env,packet->caller,a->a5,a->a6};a->result=target_16(a->a0,a->a1,a->a2,a->a3,a->a4,&c,a->a6?write_compressed:NULL);return 0;}
static int32_t (*target_17)(JNIEnv *,jobject);
static int64_t invoke_17(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_17 *a=(void*)(uintptr_t)packet->arguments;if(!target_17)return unavailable("AndroidBitmap_getDataSpace",0);if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env)return -22;a->result=target_17(a->a0,a->a1);return 0;}
static int (*target_18)(JNIEnv *,jobject,AHardwareBuffer **);
static int64_t invoke_18(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_18 *a=(void*)(uintptr_t)packet->arguments;if(!target_18)return unavailable("AndroidBitmap_getHardwareBuffer",0);if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env)return -22;a->result=target_18(a->a0,a->a1,a->a2);return 0;}
static int (*target_19)(JNIEnv *,jobject,AndroidBitmapInfo *);
static int64_t invoke_19(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_19 *a=(void*)(uintptr_t)packet->arguments;if(!target_19)return unavailable("AndroidBitmap_getInfo",0);if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env)return -22;a->result=target_19(a->a0,a->a1,a->a2);return 0;}
static int (*target_20)(JNIEnv *,jobject,void **);
static int64_t invoke_20(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_20 *a=(void*)(uintptr_t)packet->arguments;if(!target_20)return unavailable("AndroidBitmap_lockPixels",0);if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env)return -22;a->result=target_20(a->a0,a->a1,a->a2);return 0;}
static int (*target_21)(JNIEnv *,jobject);
static int64_t invoke_21(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_21 *a=(void*)(uintptr_t)packet->arguments;if(!target_21)return unavailable("AndroidBitmap_unlockPixels",0);if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env)return -22;a->result=target_21(a->a0,a->a1);return 0;}
static const KljBionicNdkTarget targets[]={
{(uintptr_t)invoke_0,sizeof(GraphicsArgs_0)},
{(uintptr_t)invoke_1,sizeof(GraphicsArgs_1)},
{(uintptr_t)invoke_2,sizeof(GraphicsArgs_2)},
{(uintptr_t)invoke_3,sizeof(GraphicsArgs_3)},
{(uintptr_t)invoke_4,sizeof(GraphicsArgs_4)},
{(uintptr_t)invoke_5,sizeof(GraphicsArgs_5)},
{(uintptr_t)invoke_6,sizeof(GraphicsArgs_6)},
{(uintptr_t)invoke_7,sizeof(GraphicsArgs_7)},
{(uintptr_t)invoke_8,sizeof(GraphicsArgs_8)},
{(uintptr_t)invoke_9,sizeof(GraphicsArgs_9)},
{(uintptr_t)invoke_10,sizeof(GraphicsArgs_10)},
{(uintptr_t)invoke_11,sizeof(GraphicsArgs_11)},
{(uintptr_t)invoke_12,sizeof(GraphicsArgs_12)},
{(uintptr_t)invoke_13,sizeof(GraphicsArgs_13)},
{(uintptr_t)invoke_14,sizeof(GraphicsArgs_14)},
{(uintptr_t)invoke_15,sizeof(GraphicsArgs_15)},
{(uintptr_t)invoke_16,sizeof(GraphicsArgs_16)},
{(uintptr_t)invoke_17,sizeof(GraphicsArgs_17)},
{(uintptr_t)invoke_18,sizeof(GraphicsArgs_18)},
{(uintptr_t)invoke_19,sizeof(GraphicsArgs_19)},
{(uintptr_t)invoke_20,sizeof(GraphicsArgs_20)},
{(uintptr_t)invoke_21,sizeof(GraphicsArgs_21)},
};
static const KljBionicNdkBindings bindings={sizeof(bindings),KLJ_BIONIC_NDK_ABI,sizeof(targets)/sizeof(*targets),0,targets};
const KljBionicNdkBindings *klj_bionic_graphics_bindings(void){return ready?&bindings:NULL;}
int klj_bionic_graphics_prepare(const KljBionicGraphicsOwner *input){if(!input||!input->resolve||!input->environment)return -22;pthread_mutex_lock(&lock);if(ready){int same=owner.resolve==input->resolve&&owner.environment==input->environment&&owner.invoke==input->invoke&&owner.context==input->context;pthread_mutex_unlock(&lock);return same?0:-16;}
void *p0=input->resolve("AHardwareBuffer_acquire");memcpy(&target_0,&p0,sizeof(target_0));
void *p1=input->resolve("AHardwareBuffer_allocate");memcpy(&target_1,&p1,sizeof(target_1));
void *p2=input->resolve("AHardwareBuffer_describe");memcpy(&target_2,&p2,sizeof(target_2));
void *p3=input->resolve("AHardwareBuffer_fromHardwareBuffer");memcpy(&target_3,&p3,sizeof(target_3));
void *p4=input->resolve("AHardwareBuffer_getId");memcpy(&target_4,&p4,sizeof(target_4));
void *p5=input->resolve("AHardwareBuffer_isSupported");memcpy(&target_5,&p5,sizeof(target_5));
void *p6=input->resolve("AHardwareBuffer_lock");memcpy(&target_6,&p6,sizeof(target_6));
void *p7=input->resolve("AHardwareBuffer_lockAndGetInfo");memcpy(&target_7,&p7,sizeof(target_7));
void *p8=input->resolve("AHardwareBuffer_lockPlanes");memcpy(&target_8,&p8,sizeof(target_8));
void *p9=input->resolve("AHardwareBuffer_readFromParcel");memcpy(&target_9,&p9,sizeof(target_9));
void *p10=input->resolve("AHardwareBuffer_recvHandleFromUnixSocket");memcpy(&target_10,&p10,sizeof(target_10));
void *p11=input->resolve("AHardwareBuffer_release");memcpy(&target_11,&p11,sizeof(target_11));
void *p12=input->resolve("AHardwareBuffer_sendHandleToUnixSocket");memcpy(&target_12,&p12,sizeof(target_12));
void *p13=input->resolve("AHardwareBuffer_toHardwareBuffer");memcpy(&target_13,&p13,sizeof(target_13));
void *p14=input->resolve("AHardwareBuffer_unlock");memcpy(&target_14,&p14,sizeof(target_14));
void *p15=input->resolve("AHardwareBuffer_writeToParcel");memcpy(&target_15,&p15,sizeof(target_15));
void *p16=input->resolve("AndroidBitmap_compress");memcpy(&target_16,&p16,sizeof(target_16));
void *p17=input->resolve("AndroidBitmap_getDataSpace");memcpy(&target_17,&p17,sizeof(target_17));
void *p18=input->resolve("AndroidBitmap_getHardwareBuffer");memcpy(&target_18,&p18,sizeof(target_18));
void *p19=input->resolve("AndroidBitmap_getInfo");memcpy(&target_19,&p19,sizeof(target_19));
void *p20=input->resolve("AndroidBitmap_lockPixels");memcpy(&target_20,&p20,sizeof(target_20));
void *p21=input->resolve("AndroidBitmap_unlockPixels");memcpy(&target_21,&p21,sizeof(target_21));
owner=*input;ready=1;pthread_mutex_unlock(&lock);return 0;}
int64_t klj_bionic_graphics_dispatch(JNIEnv *env,KljBionicNdkPacket *packet){if(!ready||!packet||packet->size!=sizeof(*packet)||packet->version!=KLJ_BIONIC_NDK_ABI||!packet->arguments)return -22;for(unsigned i=0;i<bindings.count;i++){if(packet->target!=targets[i].target)continue;if(packet->argument_size!=targets[i].argument_size)return -22;int saved=errno;errno=kl_errno_from_linux(packet->error);int64_t status=((int64_t(*)(JNIEnv*,KljBionicNdkPacket*))(uintptr_t)packet->target)(env,packet);packet->error=kl_errno_to_linux(errno);errno=saved;return status;}return -1;}
