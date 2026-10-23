/** Retain native callback payloads in their original GL context lifetime. */
export function adaptAngleBlobCallbackOwner(original){
 const replace=(text,from,to)=>{if(text.split(from).length!==2)throw Error('Original ANGLE blob callback boundary changed');return text.replace(from,to);};
 let text=replace(original,'#include "libANGLE/Context.inl.h"','#include "libANGLE/Context.inl.h"\n#include "angle-blob-callback.h"\n#include <pthread.h>\n#include <new>');
 const ownership=`
namespace klj_angle_blob_owner {
struct Owner {const gl::Context *context;KljAngleBlobOwnerV1 lease;Owner *next;};
struct Pending {const KljAngleBlobOwnerV1 *lease;bool committed;Pending *previous;};
thread_local Pending *pending=nullptr;
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
Owner *first=nullptr;
void replace(const gl::Context *context,Owner *replacement) {
    Owner *removed=nullptr;pthread_mutex_lock(&mutex);Owner **at=&first;
    while(*at){if((*at)->context==context){removed=*at;*at=removed->next;break;}at=&(*at)->next;}
    if(replacement){replacement->next=first;first=replacement;}
    pthread_mutex_unlock(&mutex);
    if(removed){removed->lease.release(removed->lease.payload);delete removed;}
}
}
`;
 text=replace(text,'namespace gl\n{',ownership+'\nnamespace gl\n{');
 text=replace(text,'Context::~Context() {}','Context::~Context() { klj_angle_blob_owner::replace(this,nullptr); }');
 text=replace(text,'    mState.getBlobCacheCallbacks() = {set, get, userParam};',`    auto request=klj_angle_blob_owner::pending;
    klj_angle_blob_owner::Owner *replacement=nullptr;
    if(request&&(set||get)){
        if(request->lease->payload&&request->lease->release)
            replacement=new(std::nothrow) klj_angle_blob_owner::Owner{this,*request->lease,nullptr};
        if(!replacement){handleError(GL_OUT_OF_MEMORY,"Native blob callback ownership allocation failed",__FILE__,__func__,__LINE__);return;}
    }
    mState.getBlobCacheCallbacks() = {set, get, userParam};
    klj_angle_blob_owner::replace(this,replacement);
    if(request)request->committed=true;`);
 text+=`
extern "C" __attribute__((visibility("default"))) int maclauncher_angle_blob_callback_owner_v1(
    KljAngleBlobInstall install,KljAngleSetBlob set,KljAngleGetBlob get,const void *user,const KljAngleBlobOwnerV1 *lease) {
    if(!install||!lease||lease->size!=sizeof(*lease)||lease->version!=1)return 0;
    klj_angle_blob_owner::Pending request{lease,false,klj_angle_blob_owner::pending};
    klj_angle_blob_owner::pending=&request;
    // The original public entry validates extension state, callback balance and
    // current context before its original context method accepts ownership.
    install(set,get,user);
    klj_angle_blob_owner::pending=request.previous;
    return request.committed?1:0;
}
`;
 return text;
}
