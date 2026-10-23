/** Bind an opaque native callback lease to the original GL debug-state owner.
 * No class layout or public GL declarations change. */
export function adaptAngleDebugCallbackOwner(original){
 const replace=(text,from,to)=>{if(text.split(from).length!==2)throw Error('Original ANGLE debug callback boundary changed');return text.replace(from,to);};
 let text=replace(original,'#include "libANGLE/Debug.h"','#include "libANGLE/Debug.h"\n#include "libGLESv2/global_state.h"\n#include "angle-debug-callback.h"\n#include <pthread.h>\n#include <new>');
 const owners=`
namespace klj_angle_debug_owner {
struct Owner {const gl::Debug *debug;KljAngleDebugOwnerV1 lease;Owner *next;};
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
struct Lock {Lock(){pthread_mutex_lock(&mutex);}~Lock(){pthread_mutex_unlock(&mutex);}};
Owner *first=nullptr;
void retire(const gl::Debug *debug) {
    Owner *removed=nullptr;
    {Lock lock;Owner **at=&first;
     while(*at){if((*at)->debug==debug){removed=*at;*at=removed->next;break;}at=&(*at)->next;}}
    if(removed){removed->lease.release(removed->lease.payload);delete removed;}
}
}
`;
 text=replace(text,'#include <tuple>\n\nnamespace\n{','#include <tuple>\n'+owners+'\nnamespace\n{');
 text=replace(text,'Debug::~Debug() {}','Debug::~Debug() { klj_angle_debug_owner::retire(this); }');
 text=replace(text,'    mCallbackUserParam = userParam;','    mCallbackUserParam = userParam;\n    klj_angle_debug_owner::retire(this);');
 text+=`
extern "C" __attribute__((visibility("default"))) int maclauncher_angle_debug_callback_owner_v1(
    KljAngleDebugInstall install,KljAngleDebugCallback callback,const void *user,const KljAngleDebugOwnerV1 *lease) {
    if(!install||!lease||lease->size!=sizeof(*lease)||lease->version!=1)return 0;
    auto context=gl::GetValidGlobalContext();
    if(!context){install(callback,user);return 0;}
    if(!callback){install(nullptr,user);return 1;}
    if(!lease->payload||!lease->release){context->handleError(GL_OUT_OF_MEMORY,"Native debug callback allocation failed",__FILE__,__func__,__LINE__);return 0;}
    const auto *debug=&context->getState().getDebug();
    auto owner=new(std::nothrow) klj_angle_debug_owner::Owner{debug,*lease,nullptr};
    if(!owner){context->handleError(GL_OUT_OF_MEMORY,"Native debug callback ownership allocation failed",__FILE__,__func__,__LINE__);return 0;}
    install(callback,user);
    if(debug->getCallback()!=callback||debug->getUserParam()!=user){delete owner;return 0;}
    {klj_angle_debug_owner::Lock lock;owner->next=klj_angle_debug_owner::first;klj_angle_debug_owner::first=owner;}
    return 1;
}
`;
 return text;
}
