#include "../../angle-blob-callback.h"
typedef struct {_Atomic unsigned references;uintptr_t set,get,user,caller;} KljBlobCallback;
static _Atomic(uintptr_t) blob_owner_address;
static KljAngleBlobOwnerInstall blob_owner(void){
    uintptr_t value=atomic_load_explicit(&blob_owner_address,memory_order_acquire);
    if(!value){void *entry=owner.resolve("maclauncher_angle_blob_callback_owner_v1");value=entry?(uintptr_t)entry:1;atomic_store_explicit(&blob_owner_address,value,memory_order_release);}
    KljAngleBlobOwnerInstall result=NULL;if(value!=1)memcpy(&result,&value,sizeof(result));return result;
}
static void blob_release(void *raw){KljBlobCallback *value=raw;if(value&&atomic_fetch_sub_explicit(&value->references,1,memory_order_acq_rel)==1)free(value);}
static int64_t blob_packet(KljBlobCallback *value,KljCallPacket *packet){
    JNIEnv *environment=NULL;jint attached=(*owner.vm)->GetEnv(owner.vm,(void**)&environment,JNI_VERSION_1_6);
    if(!((attached==JNI_OK&&environment)||(attached==JNI_EDETACHED&&!environment)))return -22;
    int saved=errno;int64_t status=owner.invoke(owner.context,environment,value->caller,packet);errno=saved;return status?status:packet->status;
}
static void blob_set(const void *key,intptr_t keySize,const void *data,intptr_t size,const void *raw){
    KljBlobCallback *value=(KljBlobCallback*)raw;atomic_fetch_add_explicit(&value->references,1,memory_order_relaxed);
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=value->set,
        .x={(uintptr_t)key,(uint64_t)(int64_t)keySize,(uintptr_t)data,(uint64_t)(int64_t)size,value->user}};
    int64_t status=blob_packet(value,&packet);if(status)fprintf(stderr,"Android graphics blob cache write callback failed (%lld)\n",(long long)status);blob_release(value);
}
static intptr_t blob_get(const void *key,intptr_t keySize,void *data,intptr_t size,const void *raw){
    KljBlobCallback *value=(KljBlobCallback*)raw;atomic_fetch_add_explicit(&value->references,1,memory_order_relaxed);
    KljCallPacket packet={.size=sizeof(packet),.version=KLJ_CALL_PACKET_VERSION,.target=value->get,
        .x={(uintptr_t)key,(uint64_t)(int64_t)keySize,(uintptr_t)data,(uint64_t)(int64_t)size,value->user}};
    int64_t status=blob_packet(value,&packet);if(status)fprintf(stderr,"Android graphics blob cache read callback failed (%lld)\n",(long long)status);intptr_t result=status?0:(intptr_t)packet.result_x[0];blob_release(value);return result;
}
static int blob_install(KljAngleBlobInstall target,uintptr_t set,uintptr_t get,uintptr_t user,uintptr_t caller){
    KljAngleBlobOwnerInstall install=blob_owner();if(!install||!owner.invoke||!owner.vm)return 0;
    KljBlobCallback *value=(set||get)?malloc(sizeof(*value)):NULL;
    if(value){atomic_init(&value->references,1);value->set=set;value->get=get;value->user=user;value->caller=caller;}
    KljAngleBlobOwnerV1 lease={sizeof(lease),1,value,blob_release};
    if(!install(target,set?blob_set:NULL,get?blob_get:NULL,(set||get)?value:(void*)user,&lease))blob_release(value);
    return 1;
}
static void blob_pointer(unsigned name,void **output,void (*query)(unsigned,void**)){
    if(!output||(name!=0x96bf&&name!=0x96ef&&name!=0x972d))return;
    void *set=NULL,*get=NULL,*raw=NULL;query(0x96ef,&set);query(0x96bf,&get);
    if(set!=(void*)blob_set||get!=(void*)blob_get)return;query(0x972d,&raw);if(!raw)return;
    KljBlobCallback *value=raw;*output=(void*)(name==0x96ef?value->set:name==0x96bf?value->get:value->user);
}
