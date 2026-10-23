#include "../../angle-debug-callback.h"
#include "call-packet.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    _Atomic unsigned references;
    uintptr_t callback,user,caller;
} KljDebugCallback;
static void debug_release(void *raw){KljDebugCallback *value=raw;if(value&&atomic_fetch_sub_explicit(&value->references,1,memory_order_acq_rel)==1)free(value);}
static void debug_callback(uint32_t source,uint32_t type,uint32_t id,uint32_t severity,int32_t length,const char *message,const void *raw){
    KljDebugCallback *value=(KljDebugCallback*)raw;
    atomic_fetch_add_explicit(&value->references,1,memory_order_relaxed);
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=value->callback,
        .x={source,type,id,severity,(uint64_t)(int64_t)length,(uintptr_t)message,value->user}};
    // ANGLE's original debug state calls synchronously on the same current
    // physical GL thread. The existing native callback entry preserves that
    // thread's Bionic TLS and Java attachment state.
    JNIEnv *environment=NULL;
    jint attached=(*owner.vm)->GetEnv(owner.vm,(void**)&environment,JNI_VERSION_1_6);
    int64_t status=((attached==JNI_OK&&environment)||(attached==JNI_EDETACHED&&!environment))
        ?owner.invoke(owner.context,environment,value->caller,&packet):-22;
    if(status||packet.status)fprintf(stderr,"Android graphics debug callback failed (%lld)\n",(long long)(status?status:packet.status));
    debug_release(value);
}
static void debug_install(KljAngleDebugInstall target,uintptr_t callback,uintptr_t user,uintptr_t caller){
    KljDebugCallback *value=callback?malloc(sizeof(*value)):NULL;
    if(value){atomic_init(&value->references,1);value->callback=callback;value->user=user;value->caller=caller;}
    KljAngleDebugOwnerV1 lease={sizeof(lease),1,value,debug_release};
    if(!owner.debug_callback(target,callback?debug_callback:NULL,callback?value:(void*)user,&lease))debug_release(value);
}
static void debug_pointer(unsigned name,void **output,void (*query)(unsigned,void**)){
    if(!output||(name!=0x8244&&name!=0x8245))return;
    void *callback=NULL;query(0x8244,&callback);if(callback!=(void*)debug_callback)return;
    void *raw=NULL;query(0x8245,&raw);if(!raw)return;
    KljDebugCallback *value=raw;*output=(void*)(name==0x8244?value->callback:value->user);
}
