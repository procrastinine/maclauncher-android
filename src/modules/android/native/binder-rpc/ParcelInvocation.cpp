#include "guest/parcel-invocation.h"
#include "ParcelTransport.h"
#include "../jni-invocation.h"
#include "../native-import-registry.h"
#include "android_os_Parcel.h"
#include "android_util_Binder.h"
#include <binder/Binder.h>
#include <binder/RpcServer.h>
#include <binder/unique_fd.h>
#include <sys/socket.h>
#include <memory>
#include <vector>
#include <cstring>
#include <map>
#include <mutex>
#include <fcntl.h>

namespace {
using namespace android;
struct Lease {
    jobject object=nullptr;
    Parcel *parcel=nullptr;
    std::unique_ptr<Parcel> owned;
};
struct State {
    uint64_t guest;
    KljParcelSynchronize callback;
    uint64_t token=0;
    std::vector<std::unique_ptr<Lease>> leases;
};
struct Channel {
    std::mutex mutex;
    sp<RpcServer> server;
    binder::unique_fd bootstrap;
    std::map<uint64_t,std::weak_ptr<State>> active;
    uint64_t next=0;
    bool closed=false;
};
[[clang::no_destroy]] Channel channel;
using Ownership=std::shared_ptr<State>;
void error(JNIEnv *env,const char *message){
    if(env->ExceptionCheck())return;
    jclass type=env->FindClass("java/lang/IllegalStateException");
    if(type){env->ThrowNew(type,message);env->DeleteLocalRef(type);}
}
void status(JNIEnv *env,status_t result){if(result!=OK&&!env->ExceptionCheck())signalExceptionForError(env,nullptr,result);}
extern const KljJniInvocationResource operations;
State *state(JNIEnv *env){
    auto *scope=klj_jni_current_invocation();
    if(!scope||scope->operations!=&operations){error(env,"Parcel access requires an active native invocation");return nullptr;}
    return static_cast<Ownership *>(scope->resource)->get();
}
Lease *lease(State& owner,uint64_t id){return id&&id<=owner.leases.size()?owner.leases[size_t(id-1)].get():nullptr;}
class Endpoint final:public BBinder {
public:
    status_t onTransact(uint32_t code,const Parcel& request,Parcel* reply,uint32_t) override {
        uint64_t token,id;status_t result=request.readUint64(&token);if(result!=OK)return result;
        if((result=request.readUint64(&id))!=OK)return result;
        Ownership owner;
        {std::lock_guard lock(channel.mutex);auto entry=channel.active.find(token);if(entry!=channel.active.end())owner=entry->second.lock();}
        if(!owner)return DEAD_OBJECT;
        auto *entry=lease(*owner,id);if(!entry||!entry->parcel)return BAD_VALUE;
        if(code==KLJ_PARCEL_PUSH)return HostParcelTransport::readSnapshot(request,entry->parcel);
        if(code==KLJ_PARCEL_PULL&&reply)return HostParcelTransport::writeSnapshot(*entry->parcel,reply);
        return UNKNOWN_TRANSACTION;
    }
};
int synchronize(void *opaque,JNIEnv *env,int toJava){
    auto& owner=**static_cast<Ownership *>(opaque);
    // Java may recycle a Parcel directly during a callback. Resolve its current
    // native owner before asking the guest to refresh; never reuse a freed ptr.
    if(!toJava)for(auto& entry:owner.leases)if(entry&&entry->object){
        entry->parcel=parcelForJavaObject(env,entry->object);if(env->ExceptionCheck())return 0;
    }
    status_t result=owner.callback(owner.guest,toJava?KLJ_PARCEL_PUSH:KLJ_PARCEL_PULL);
    status(env,result);return result==OK&&!env->ExceptionCheck();
}
void destroy(void *opaque,JNIEnv *env){
    std::unique_ptr<Ownership> ownership(static_cast<Ownership *>(opaque));auto& owner=*ownership;
    status_t result=owner->callback(owner->guest,KLJ_PARCEL_DESTROY);
    {std::lock_guard lock(channel.mutex);channel.active.erase(owner->token);}
    for(auto& entry:owner->leases)if(entry&&entry->object)env->DeleteGlobalRef(entry->object);
    status(env,result);
}
const KljJniInvocationResource operations={synchronize,destroy};
}

extern "C" uint64_t klj_parcel_scope_begin(JNIEnv *env){
    if(klj_jni_current_invocation())return 0;
    auto *scope=new(std::nothrow) KljJniInvocation;
    if(!scope){status(env,android::NO_MEMORY);return 0;}
    if(!klj_jni_invocation_enter(scope,env)){klj_jni_invocation_leave(scope);delete scope;return 0;}
    return reinterpret_cast<uintptr_t>(scope);
}
extern "C" void klj_parcel_scope_end(JNIEnv *,uint64_t token){
    if(!token)return;auto *scope=reinterpret_cast<KljJniInvocation *>(uintptr_t(token));
    if(scope!=klj_jni_current_invocation())return;
    klj_jni_invocation_leave(scope);delete scope;
}
extern "C" uint64_t klj_parcel_scope_state(JNIEnv *){
    auto *scope=klj_jni_current_invocation();
    return scope&&scope->operations==&operations?(*static_cast<Ownership *>(scope->resource))->guest:0;
}
extern "C" int klj_parcel_transport_socket(void){
    try{
        std::lock_guard lock(channel.mutex);if(channel.closed){errno=EPIPE;return -1;}
        if(channel.bootstrap.ok())return fcntl(channel.bootstrap.get(),F_DUPFD_CLOEXEC,0);
        int sockets[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)<0)return -1;
        android::binder::unique_fd server(sockets[0]),client(sockets[1]);
        auto owner=android::RpcServer::make();if(!owner){errno=ENOMEM;return -1;}
        owner->setMaxThreads(4);
        owner->setSupportedFileDescriptorTransportModes({android::RpcSession::FileDescriptorTransportMode::UNIX});
        owner->setPerSessionRootObject([root=android::sp<Endpoint>::make()](android::wp<android::RpcSession> weak,const void*,size_t)->android::sp<android::IBinder>{
            auto session=weak.promote();if(!session)return nullptr;
            session->setHostLocalBridge();return root;
        });
        auto result=owner->setupUnixDomainSocketBootstrapServer(std::move(server));
        if(result!=android::OK){errno=-result;return -1;}
        owner->start();channel.server=std::move(owner);channel.bootstrap=std::move(client);
        return fcntl(channel.bootstrap.get(),F_DUPFD_CLOEXEC,0);
    }catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
     catch(const std::system_error& failure){errno=failure.code().value();return -1;}
}
void kljCloseParcelTransport(){
    sp<android::RpcServer> server;
    {std::lock_guard lock(channel.mutex);channel.closed=true;channel.bootstrap.reset();server=std::move(channel.server);}
    if(server)(void)server->shutdown();
}
extern "C" uint64_t klj_parcel_scope_open(JNIEnv *env,uint64_t guest,KljParcelSynchronize callback){
    if(!guest||!callback||!klj_jni_current_invocation()){error(env,"Parcel transport requires an active native invocation");return 0;}
    try{
        auto ownership=std::make_unique<Ownership>(std::make_shared<State>());auto& owner=*ownership;
        owner->guest=guest;owner->callback=callback;
        {std::lock_guard lock(channel.mutex);owner->token=++channel.next;channel.active.emplace(owner->token,owner);}
        if(!klj_jni_invocation_attach(ownership.get(),&operations)){
            {std::lock_guard lock(channel.mutex);channel.active.erase(owner->token);}
            error(env,"Parcel invocation already owns a transport");return 0;
        }
        uint64_t token=owner->token;ownership.release();return token;
    }catch(const std::bad_alloc&){status(env,android::NO_MEMORY);return 0;}
}
extern "C" uint64_t klj_parcel_scope_lease(JNIEnv *env,jobject object,int create){
    auto *owner=state(env);if(!owner||!object)return 0;
    for(size_t i=0;i<owner->leases.size();i++){
        auto& value=owner->leases[i];if(value&&value->object&&env->IsSameObject(object,value->object))return i+1;
    }
    if(!create)return 0;
    auto parcel=android::parcelForJavaObject(env,object);if(!parcel||env->ExceptionCheck())return 0;
    try{
        auto value=std::make_unique<Lease>();value->object=env->NewGlobalRef(object);if(!value->object)return 0;
        value->parcel=parcel;
        try{owner->leases.push_back(std::move(value));}
        catch(...){env->DeleteGlobalRef(value->object);throw;}
        return owner->leases.size();
    }catch(const std::bad_alloc&){status(env,android::NO_MEMORY);return 0;}
}
extern "C" uint64_t klj_parcel_scope_create(JNIEnv *env){
    auto *owner=state(env);if(!owner)return 0;
    try{auto value=std::make_unique<Lease>();value->owned=std::make_unique<android::Parcel>();value->parcel=value->owned.get();owner->leases.push_back(std::move(value));return owner->leases.size();}
    catch(const std::bad_alloc&){status(env,android::NO_MEMORY);return 0;}
}
extern "C" void klj_parcel_scope_release(JNIEnv *env,uint64_t id){
    auto *owner=state(env);if(!owner)return;auto *entry=lease(*owner,id);if(!entry)return;
    if(entry->object)env->DeleteGlobalRef(entry->object);owner->leases[size_t(id-1)].reset();
}
extern "C" jlong klj_parcel_scope_pointer(JNIEnv *env,uint64_t id){
    auto *owner=state(env);auto *entry=owner?lease(*owner,id):nullptr;return entry?reinterpret_cast<jlong>(entry->parcel):0;
}
extern "C" uint64_t klj_parcel_scope_binder(JNIEnv *env,jobject object){
    uint64_t id=klj_parcel_scope_create(env);if(!id)return 0;
    auto *owner=state(env);auto *entry=lease(*owner,id);
    auto binder=android::ibinderForJavaObject(env,object);
    if(env->ExceptionCheck()){klj_parcel_scope_release(env,id);return 0;}
    auto result=entry->parcel->writeStrongBinder(binder);status(env,result);
    if(result!=android::OK){klj_parcel_scope_release(env,id);return 0;}
    entry->parcel->setDataPosition(0);return id;
}
extern "C" jobject klj_parcel_scope_java_binder(JNIEnv *env,uint64_t id){
    auto *owner=state(env);auto *entry=owner?lease(*owner,id):nullptr;if(!entry)return nullptr;
    entry->parcel->setDataPosition(0);android::sp<android::IBinder> binder;
    auto result=entry->parcel->readNullableStrongBinder(&binder);status(env,result);
    if(result!=android::OK)return nullptr;
    // Original JavaBBinder returns its borrowed global reference, while a new
    // BinderProxy returns a local reference. The ABI caller owns one local JNI
    // result in either case; retiring it must never delete Binder's global.
    jobject original=android::javaObjectForIBinder(env,binder);if(!original)return nullptr;
    jobject local=env->NewLocalRef(original);
    if(env->GetObjectRefType(original)==JNILocalRefType)env->DeleteLocalRef(original);
    return local;
}
bool kljInstallParcelInvocation(){
    return klj_register_import_resolver([](const char *name)->void*{
#define IMPORT(function) if(!std::strcmp(name,#function))return reinterpret_cast<void *>(&function)
        IMPORT(klj_parcel_scope_begin);IMPORT(klj_parcel_scope_end);IMPORT(klj_parcel_scope_state);
        IMPORT(klj_parcel_scope_open);IMPORT(klj_parcel_scope_lease);IMPORT(klj_parcel_scope_create);
        IMPORT(klj_parcel_scope_release);IMPORT(klj_parcel_scope_pointer);
        IMPORT(klj_parcel_transport_socket);IMPORT(klj_parcel_scope_binder);IMPORT(klj_parcel_scope_java_binder);
#undef IMPORT
        return nullptr;
    })!=0;
}
