#include "ParcelTransport.h"
#include "hotspot-jni.h"
#include "core_jni_helpers.h"
#include "android_util_Binder.h"
#include "rpc-identity.h"
#include <binder/IPCThreadState.h>
#include <binder/BpBinder.h>
#include <binder/RpcServer.h>
#include <binder/RpcSession.h>
#include <binder/RpcTransportRaw.h>
#include <binder/unique_fd.h>
#include <android_runtime/AndroidRuntime.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifdef __APPLE__
#include "../process-ids.h"
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <cstring>
#include <shared_mutex>
#include <atomic>

namespace {
using namespace android;
struct Peer {std::string packageName,processName,securityContext;pid_t pid;uid_t uid;};
std::mutex peersMutex;
std::map<const RpcSession*,std::shared_ptr<const Peer>> peers;
thread_local const Peer* callingPeer;
jclass peerClass;
jmethodID peerConstructor,packageNameMethod,processNameMethod,pidMethod,uidMethod,securityContextMethod;
struct Owner {
    sp<RpcSession> session;
    sp<RpcServer> server;
    sp<IBinder> root;
    std::string socketPath;
    bool acceptedSession=false;
    ~Owner(){
        if(server)(void)server->shutdown();
        // RpcServer owns joining accepted-session threads; only client
        // sessions install the original per-session shutdown waiter.
        if(session)(void)session->shutdownAndWait(!acceptedSession);
        if(!socketPath.empty())unlink(socketPath.c_str());
    }
};
std::mutex ownersMutex;
std::shared_mutex ownerLifetime;
std::map<Owner*,std::shared_ptr<Owner>> owners;
std::atomic<bool> retiring=false;
bool available(JNIEnv* env){if(!retiring.load(std::memory_order_acquire))return true;jniThrowException(env,"java/lang/IllegalStateException","Android Binder runtime is closed");return false;}
jlong publishOwner(std::shared_ptr<Owner> owner){auto* pointer=owner.get();std::lock_guard lock(ownersMutex);owners.emplace(pointer,std::move(owner));return reinterpret_cast<jlong>(pointer);}
void closeOwner(jlong pointer){std::shared_ptr<Owner> owner;{std::lock_guard lock(ownersMutex);auto found=owners.find(reinterpret_cast<Owner*>(pointer));if(found==owners.end())return;owner=std::move(found->second);owners.erase(found);}}
std::shared_ptr<Peer> readPeer(JNIEnv* env,jobject value){
    auto peer=std::make_shared<Peer>();
    ScopedLocalRef<jstring> package(env,static_cast<jstring>(env->CallObjectMethod(value,packageNameMethod)));if(env->ExceptionCheck())return {};
    ScopedUtfChars packageChars(env,package.get());if(!packageChars.c_str())return {};
    ScopedLocalRef<jstring> process(env,static_cast<jstring>(env->CallObjectMethod(value,processNameMethod)));if(env->ExceptionCheck())return {};
    ScopedUtfChars processChars(env,process.get());if(!processChars.c_str())return {};
    peer->packageName=packageChars.c_str();peer->processName=processChars.c_str();
    peer->pid=env->CallIntMethod(value,pidMethod);if(env->ExceptionCheck())return {};
    peer->uid=env->CallIntMethod(value,uidMethod);if(env->ExceptionCheck())return {};
    ScopedLocalRef<jstring> context(env,static_cast<jstring>(env->CallObjectMethod(value,securityContextMethod)));if(env->ExceptionCheck())return {};
    ScopedUtfChars contextChars(env,context.get());if(!contextChars.c_str())return {};peer->securityContext=contextChars.c_str();
    return peer;
}
binder::unique_fd socketDescriptor(JNIEnv* env,jint descriptor){
    sockaddr_storage address{};socklen_t size=sizeof(address);int type=0;socklen_t typeSize=sizeof(type);
    if(getsockname(descriptor,reinterpret_cast<sockaddr*>(&address),&size)<0||getsockopt(descriptor,SOL_SOCKET,SO_TYPE,&type,&typeSize)<0){jniThrowIOException(env,errno);return {};}
    if(address.ss_family!=AF_UNIX||type!=SOCK_STREAM){jniThrowException(env,"java/lang/IllegalArgumentException","Binder bootstrap requires a Unix stream socket");return {};}
    int copy=fcntl(descriptor,F_DUPFD_CLOEXEC,0);if(copy<0)jniThrowIOException(env,errno);return binder::unique_fd(copy);
}
void ioStatus(JNIEnv* env,status_t status){if(status!=OK)jniThrowIOException(env,status<0?-status:EIO);}
void connectStatus(JNIEnv* env,status_t status){
    // Only transport loss is optional. Invalid descriptors, protocol arguments,
    // allocation failures and permission errors retain their original failures.
    const int error=status<0?-status:EIO;
    switch(error){
        case EPIPE:case ECONNRESET:case ECONNREFUSED:case ECONNABORTED:case ENOTCONN:case ETIMEDOUT:
            jniThrowException(env,"maclauncher/android/framework/HostBinderRpc$PeerUnavailableException",strerror(error));break;
        default:ioStatus(env,status);break;
    }
}
struct ProcessRegistry {
    jobject object;
    jmethodID expected,attach;
    ProcessRegistry(JNIEnv* env,jobject value):object(env->NewGlobalRef(value)),expected(nullptr),attach(nullptr){
        ScopedLocalRef<jclass> type(env,env->GetObjectClass(value));
        if(type.get()){
            expected=env->GetMethodID(type.get(),"expectedIdentity","(I)Lmaclauncher/android/framework/HostBinderRpc$Peer;");
            if(expected)attach=env->GetMethodID(type.get(),"attachIncoming","(JLmaclauncher/android/framework/HostBinderRpc$Peer;)V");
        }
    }
    ~ProcessRegistry(){if(object)AndroidRuntime::getJNIEnv()->DeleteGlobalRef(object);}
    std::shared_ptr<const Peer> authenticate(int socket)const{
        if(retiring.load(std::memory_order_acquire))return {};
        pid_t pid=0;uid_t uid=0;gid_t gid=0;socklen_t size=sizeof(pid);
        if(getsockopt(socket,SOL_LOCAL,LOCAL_PEERPID,&pid,&size)<0||size!=sizeof(pid)||
            getpeereid(socket,&uid,&gid)<0||uid!=getuid())return {};
        // Kernel transport credentials authenticate the physical process;
        // Binder carries its live Android lease identity to framework code.
#ifdef __APPLE__
        int logical=klj_process_id_from_host(pid);if(logical<0)return {};
#else
        int logical=pid; // Original Linux SO_PEERCRED already carries guest IDs.
#endif
        JNIEnv* env=AndroidRuntime::getJNIEnv();
        if(!env)return {};
        ScopedLocalRef<jobject> identity(env,env->CallObjectMethod(object,expected,static_cast<jint>(logical)));
        if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();return {};}
        if(!identity.get())return {};
        auto peer=readPeer(env,identity.get());
        if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();return {};}
        return peer;
    }
    bool adopt(const sp<RpcSession>& session,const sp<IBinder>& root,const std::shared_ptr<const Peer>& peer)const{
        std::shared_lock lifetime(ownerLifetime);if(retiring.load(std::memory_order_acquire))return false;
        JNIEnv* env=AndroidRuntime::getJNIEnv();
        if(!env)return false;
        auto failed=[&](){if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();}return false;};
        ScopedLocalRef<jstring> package(env,env->NewStringUTF(peer->packageName.c_str()));
        if(!package.get())return failed();
        ScopedLocalRef<jstring> process(env,env->NewStringUTF(peer->processName.c_str()));
        if(!process.get())return failed();
        ScopedLocalRef<jstring> context(env,env->NewStringUTF(peer->securityContext.c_str()));
        if(!context.get())return failed();
        ScopedLocalRef<jobject> identity(env,env->NewObject(peerClass,peerConstructor,package.get(),process.get(),peer->pid,peer->uid,context.get()));
        if(!identity.get())return failed();
        auto owner=std::make_shared<Owner>();owner->session=session;owner->root=root;owner->acceptedSession=true;
        // attachIncoming consumes ownership even when enrollment was retired
        // during native handshake; its Connection then schedules retirement.
        env->CallVoidMethod(object,attach,publishOwner(std::move(owner)),identity.get());
        if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();return false;}
        return true;
    }
};
// A connection-establishment thread creates its transport before the original
// per-session root callback. Only immutable credentials cross that boundary;
// all sockets and sessions remain owned by original RpcServer/RpcSession.
thread_local std::shared_ptr<const Peer> acceptedPeer;
}
namespace android {
class HostAuthenticatedTransportCtx final:public RpcTransportCtx {
    std::unique_ptr<RpcTransportCtx> original;
    std::shared_ptr<ProcessRegistry> registry;
public:
    HostAuthenticatedTransportCtx(std::unique_ptr<RpcTransportCtx> value,std::shared_ptr<ProcessRegistry> owner):original(std::move(value)),registry(std::move(owner)){}
    std::unique_ptr<RpcTransport> newTransport(RpcTransportFd socket,FdTrigger* trigger)const override{
        acceptedPeer=registry->authenticate(socket.fd.get());
        if(!acceptedPeer)return nullptr;
        return original->newTransport(std::move(socket),trigger);
    }
    std::vector<uint8_t> getCertificate(RpcCertificateFormat format)const override{return original->getCertificate(format);}
};
}
namespace {
class PlatformTransportFactory final:public RpcTransportCtxFactory {
    std::unique_ptr<RpcTransportCtxFactory> original=RpcTransportCtxFactoryRaw::make();
    std::shared_ptr<ProcessRegistry> registry;
public:
    explicit PlatformTransportFactory(std::shared_ptr<ProcessRegistry> owner):registry(std::move(owner)){}
    std::unique_ptr<RpcTransportCtx> newServerCtx()const override{return std::make_unique<HostAuthenticatedTransportCtx>(original->newServerCtx(),registry);}
    std::unique_ptr<RpcTransportCtx> newClientCtx()const override{return original->newClientCtx();}
    const char* toCString()const override{return "authenticated Unix";}
};
}
int kljRegisterBinderRpc(JNIEnv* env){
    jclass type=env->FindClass("maclauncher/android/framework/HostBinderRpc$Peer");if(!type)return -1;
    peerClass=static_cast<jclass>(env->NewGlobalRef(type));env->DeleteLocalRef(type);if(!peerClass)return -1;
    peerConstructor=env->GetMethodID(peerClass,"<init>","(Ljava/lang/String;Ljava/lang/String;IILjava/lang/String;)V");if(!peerConstructor)return -1;
    packageNameMethod=env->GetMethodID(peerClass,"packageName","()Ljava/lang/String;");if(!packageNameMethod)return -1;
    processNameMethod=env->GetMethodID(peerClass,"processName","()Ljava/lang/String;");if(!processNameMethod)return -1;
    pidMethod=env->GetMethodID(peerClass,"pid","()I");if(!pidMethod)return -1;
    uidMethod=env->GetMethodID(peerClass,"uid","()I");if(!uidMethod)return -1;
    securityContextMethod=env->GetMethodID(peerClass,"securityContext","()Ljava/lang/String;");return securityContextMethod?0:-1;
}
void kljSealBinderRpc(){retiring.store(true,std::memory_order_release);}
void kljCloseBinderRpc(){
    std::unique_lock lifetime(ownerLifetime);
    std::map<Owner*,std::shared_ptr<Owner>> retired;
    {std::lock_guard lock(ownersMutex);retired.swap(owners);}
    lifetime.unlock();
    // Each original owner drains its server/session and releases its Binder
    // roots here, before the JVM begins retiring callback environments.
    retired.clear();
}
void kljReleaseBinderRpcMetadata(JNIEnv* env){if(peerClass){env->DeleteGlobalRef(peerClass);peerClass=nullptr;}}
namespace android {
bool klj_rpc_authorize_connection(const sp<RpcSession>& session){
    auto actual=std::exchange(acceptedPeer,{});
    if(!actual)return true; // Inherited bootstrap channels already have supervisor identity.
    std::lock_guard lock(peersMutex);auto found=peers.find(session.get());
    return found!=peers.end()&&found->second->pid==actual->pid&&found->second->uid==actual->uid;
}
void klj_rpc_forget(const RpcSession* session){std::lock_guard lock(peersMutex);peers.erase(session);}
status_t klj_rpc_transact(const sp<RpcSession>& session,const sp<IBinder>& target,uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags){
    if(retiring.load(std::memory_order_acquire))return DEAD_OBJECT;
    std::shared_ptr<const Peer> peer;{std::lock_guard lock(peersMutex);auto entry=peers.find(session.get());if(entry!=peers.end())peer=entry->second;}
    struct Scope {const Peer* previous;Scope(const Peer* value):previous(callingPeer){callingPeer=value;}~Scope(){callingPeer=previous;}} scope(peer.get());
    auto dispatch=[&](const Parcel& payload,Parcel* result,uint32_t callFlags) {
        return peer ? IPCThreadState::self()->transactRpcWithIdentity(peer->pid,peer->uid,peer->securityContext.c_str(),target,code,payload,result,callFlags)
                    : target->transact(code,payload,result,callFlags);
    };
    // Transport envelopes are decoded before any original Binder endpoint:
    // Java, C++ and NDK/Rust all receive the same original native Parcel.
    if(!(flags&kHostParcelEnvelope))return dispatch(data,reply,flags);
    Parcel original,originalReply;
    if(flags&IBinder::FLAG_CLEAR_BUF){original.markSensitive();originalReply.markSensitive();}
    status_t status=HostParcelTransport::decode(data,&original);
    if(status!=OK)return status;
    status=dispatch(original,reply?&originalReply:nullptr,flags&~kHostParcelEnvelope);
    if(reply){status_t encoded=HostParcelTransport::encodeReply(data,originalReply,reply);if(encoded!=OK)return encoded;}
    return status;
}
}
extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostBinderRpc_ownerOf(JNIEnv* env,jclass,jobject object){
    auto binder=android::ibinderForJavaObject(env,object);if(!binder)return nullptr;
    auto remote=binder->remoteBinder();if(!remote||!remote->isRpcBinder())return nullptr;
    std::shared_ptr<const Peer> peer;
    {std::lock_guard lock(peersMutex);auto entry=peers.find(remote->hostRpcSession().get());if(entry!=peers.end())peer=entry->second;}
    if(!peer)return nullptr;
    ScopedLocalRef<jstring> package(env,env->NewStringUTF(peer->packageName.c_str()));if(!package.get())return nullptr;
    ScopedLocalRef<jstring> process(env,env->NewStringUTF(peer->processName.c_str()));if(!process.get())return nullptr;
    ScopedLocalRef<jstring> context(env,env->NewStringUTF(peer->securityContext.c_str()));if(!context.get())return nullptr;
    return env->NewObject(peerClass,peerConstructor,package.get(),process.get(),static_cast<jint>(peer->pid),static_cast<jint>(peer->uid),context.get());
}
extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostBinderRpc_callingPeer(JNIEnv* env,jclass){
    if(!callingPeer)return nullptr;
    // clearCallingIdentity switches subsequent local service work back to this
    // process. Keep transport authentication, but do not misattribute that work
    // to the remote caller until the original Binder identity is restored.
    auto* identity=android::IPCThreadState::self();
    if(identity->getCallingPid()!=callingPeer->pid||identity->getCallingUid()!=callingPeer->uid)return nullptr;
    ScopedLocalRef<jstring> package(env,env->NewStringUTF(callingPeer->packageName.c_str()));if(!package.get())return nullptr;
    ScopedLocalRef<jstring> process(env,env->NewStringUTF(callingPeer->processName.c_str()));if(!process.get())return nullptr;
    ScopedLocalRef<jstring> context(env,env->NewStringUTF(callingPeer->securityContext.c_str()));if(!context.get())return nullptr;
    return env->NewObject(peerClass,peerConstructor,package.get(),process.get(),static_cast<jint>(callingPeer->pid),static_cast<jint>(callingPeer->uid),context.get());
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostBinderRpc_connectNative(JNIEnv* env,jclass,jint descriptor,jobject identity,jint threads){
    std::shared_lock lifetime(ownerLifetime);if(!available(env))return 0;
    auto socket=socketDescriptor(env,descriptor);if(!socket.ok())return 0;
    auto peer=readPeer(env,identity);if(!peer)return 0;
    auto owner=std::make_shared<Owner>();auto session=RpcSession::make();if(!session){jniThrowException(env,"java/lang/OutOfMemoryError","Binder RPC session allocation failed");return 0;}
    session->setMaxIncomingThreads(threads);session->setFileDescriptorTransportMode(RpcSession::FileDescriptorTransportMode::UNIX);
    {std::lock_guard lock(peersMutex);peers[session.get()]=peer;}
    status_t status=session->setupUnixDomainSocketBootstrapClient(std::move(socket));if(status!=OK){connectStatus(env,status);return 0;}
    owner->session=session;owner->root=session->getRootObject();if(!owner->root){jniThrowException(env,"java/io/IOException","Binder RPC peer has no root object");return 0;}
    return publishOwner(std::move(owner));
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostBinderRpc_serveNative(JNIEnv* env,jclass,jint descriptor,jobject identity,jobject root,jint threads){
    std::shared_lock lifetime(ownerLifetime);if(!available(env))return 0;
    auto socket=socketDescriptor(env,descriptor);if(!socket.ok())return 0;
    auto peer=readPeer(env,identity);if(!peer)return 0;
    auto owner=std::make_shared<Owner>();owner->root=ibinderForJavaObject(env,root);if(!owner->root)return 0;
    auto server=RpcServer::make();if(!server){jniThrowException(env,"java/lang/OutOfMemoryError","Binder RPC server allocation failed");return 0;}
    server->setMaxThreads(threads);server->setSupportedFileDescriptorTransportModes({RpcSession::FileDescriptorTransportMode::UNIX});
    server->setPerSessionRootObject([peer,root=owner->root](wp<RpcSession> weak,const void*,size_t){auto session=weak.promote();if(!session)return sp<IBinder>{};{std::lock_guard lock(peersMutex);peers[session.get()]=peer;}return root;});
    status_t status=server->setupUnixDomainSocketBootstrapServer(std::move(socket));if(status!=OK){ioStatus(env,status);return 0;}
    try{server->start();}
    catch(const std::system_error& error){jniThrowIOException(env,error.code().value());return 0;}
    owner->server=server;return publishOwner(std::move(owner));
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostBinderRpc_connectPathNative(JNIEnv* env,jclass,jstring name,jobject identity,jint threads){
    std::shared_lock lifetime(ownerLifetime);if(!available(env))return 0;
    ScopedUtfChars path(env,name);if(!path.c_str())return 0;
    auto peer=readPeer(env,identity);if(!peer)return 0;
    auto owner=std::make_shared<Owner>();auto session=RpcSession::make();
    if(!session){jniThrowException(env,"java/lang/OutOfMemoryError","Binder RPC session allocation failed");return 0;}
    session->setMaxIncomingThreads(threads);session->setFileDescriptorTransportMode(RpcSession::FileDescriptorTransportMode::UNIX);
    {std::lock_guard lock(peersMutex);peers[session.get()]=peer;}
    status_t status=session->setupUnixDomainClient(path.c_str());if(status!=OK){connectStatus(env,status);return 0;}
    owner->session=session;owner->root=session->getRootObject();
    if(!owner->root){jniThrowException(env,"java/io/IOException","Binder RPC peer has no root object");return 0;}
    return publishOwner(std::move(owner));
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostBinderRpc_listenNative(JNIEnv* env,jclass,jstring name,jobject root,jobject processes,jint threads){
    std::shared_lock lifetime(ownerLifetime);if(!available(env))return 0;
    ScopedUtfChars path(env,name);if(!path.c_str())return 0;
    sockaddr_un address{};address.sun_family=AF_UNIX;address.sun_len=sizeof(address);
    if(strlen(path.c_str())>=sizeof(address.sun_path)){jniThrowIOException(env,ENAMETOOLONG);return 0;}
    strcpy(address.sun_path,path.c_str());
    auto registry=std::make_shared<ProcessRegistry>(env,processes);if(!registry->object||!registry->expected||!registry->attach)return 0;
    auto owner=std::make_shared<Owner>();owner->root=ibinderForJavaObject(env,root);if(!owner->root)return 0;
    binder::unique_fd socket(::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
    if(!socket.ok()||bind(socket.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))<0){jniThrowIOException(env,errno);return 0;}
    owner->socketPath=path.c_str();
    if(chmod(path.c_str(),0600)<0||listen(socket.get(),SOMAXCONN)<0){jniThrowIOException(env,errno);return 0;}
    auto server=RpcServer::make(std::make_unique<PlatformTransportFactory>(registry));
    if(!server){jniThrowException(env,"java/lang/OutOfMemoryError","Binder RPC server allocation failed");return 0;}
    server->setMaxThreads(threads);server->setSupportedFileDescriptorTransportModes({RpcSession::FileDescriptorTransportMode::UNIX});
    server->setPerSessionRootObject([root=owner->root,registry](wp<RpcSession> weak,const void*,size_t){
        auto session=weak.promote();auto peer=acceptedPeer;
        if(!session||!peer)return sp<IBinder>{};
        {std::lock_guard lock(peersMutex);peers[session.get()]=peer;}
        return registry->adopt(session,root,peer)?root:sp<IBinder>{};
    });
    status_t status=server->setupExternalServer(std::move(socket));if(status!=OK){ioStatus(env,status);return 0;}
    try{server->start();}
    catch(const std::system_error& error){jniThrowIOException(env,error.code().value());return 0;}
    owner->server=server;return publishOwner(std::move(owner));
}
extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostBinderRpc_rootNative(JNIEnv* env,jclass,jlong pointer){
    std::shared_lock lifetime(ownerLifetime);
    sp<IBinder> root;{std::lock_guard lock(ownersMutex);auto found=owners.find(reinterpret_cast<Owner*>(pointer));if(found!=owners.end())root=found->second->root;}
    if(!root){jniThrowException(env,"java/lang/IllegalStateException","Android Binder session is closed");return nullptr;}return javaObjectForIBinder(env,root);
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostBinderRpc_closeNative(JNIEnv*,jclass,jlong pointer){std::shared_lock lifetime(ownerLifetime);closeOwner(pointer);}
