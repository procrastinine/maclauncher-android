#include "resolver-guest.h"
#include "bridge.h"
#include <netd_resolv/resolv.h>
#include <aidl/android/net/IDnsResolver.h>
#include <android/binder_manager.h>
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <vector>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace {
using aidl::android::net::IDnsResolver;
std::shared_ptr<IDnsResolver> resolver;
std::atomic<unsigned> current;
std::mutex configuration;
bool networkAllowed;
int controlSocket=-1;
bool permission(const char*name){return android::checkCallingPermission(android::String16(name));}
void context(unsigned requested,uid_t uid,android_net_context*out){
 const auto net=requested==NETID_UNSET?current.load(std::memory_order_acquire):requested;
 *out={net,MARK_UNSET,net,MARK_UNSET,uid,0,NET_CONTEXT_INVALID_PID};
}
void message(const char*text){fprintf(stderr,"Android resolver: %s\n",text);}
int tag(int,uint32_t,uid_t,pid_t){return -EOPNOTSUPP;}
bool evaluate(const android_net_context&,const char*name){
 if(!networkAllowed)return false;
 KljArtNetwork request{};request.operation=7;request.buffer=reinterpret_cast<uintptr_t>(name);request.capacity=name?strlen(name)+1:0;
 return klj_art_host(6,&request,sizeof(request))==0&&request.allowed;
}
const ResolverNetdCallbacks callbacks{permission,context,message,tag,evaluate};
int result(const ndk::ScopedAStatus&status){if(status.isOk())return 0;const int specific=status.getServiceSpecificError();return specific?-(specific<0?-specific:specific):-EIO;}
}
bool klj_art_resolver_prepare(){
 // The socket is confined to this enrolled app composition's private mount.
 // Original dnsproxyd authenticates each client's real guest UID/PID.
 sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/dnsproxyd");
 int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0){perror("Resolver socket");return false;}
 // Like init's control-socket creation, set the exact initial mode before
 // any worker exists. This needs no later path-based metadata mutation.
 mode_t previous=umask(0111);int status=bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address));int saved=errno;umask(previous);
 if(status){close(fd);errno=saved;perror("Resolver socket publication");return false;}
 char number[32];snprintf(number,sizeof(number),"%d",fd);if(setenv("ANDROID_SOCKET_dnsproxyd",number,1)){close(fd);return false;}
 controlSocket=fd;return true;
}
bool klj_art_resolver_start(bool allowed,bool allowAds){
 if(controlSocket<0){errno=EBADF;return false;}
 KljArtNetwork access{};access.operation=8;access.allowed=allowed;
 if(klj_art_host(6,&access,sizeof(access))){errno=EPERM;return false;}
 networkAllowed=allowed;KljArtNetwork policy{};policy.operation=5;policy.allowed=allowAds;
 auto policyStatus=klj_art_host(6,&policy,sizeof(policy));if(policyStatus){fprintf(stderr,"Resolver policy initialization: %lld\n",static_cast<long long>(policyStatus));return false;}
 // Keep the matching APEX's complete implementation and its own dependencies.
 void*library=dlopen("libnetd_resolv.so",RTLD_NOW|RTLD_LOCAL);
 if(!library){fprintf(stderr,"Original resolver load: %s\n",dlerror());return false;}
 auto initialize=reinterpret_cast<bool(*)(const ResolverNetdCallbacks*)>(dlsym(library,"resolv_init"));
 if(!initialize||!initialize(&callbacks)){fprintf(stderr,"Original resolver initialization failed\n");return false;}
 resolver=IDnsResolver::fromBinder(ndk::SpAIBinder(AServiceManager_checkService("dnsresolver")));
 if(!resolver)fprintf(stderr,"Original resolver service publication failed\n");return resolver!=nullptr;
}
int klj_art_resolver_network(uint32_t netId,int state){
 std::lock_guard<std::mutex> guard(configuration);if(!resolver)return -ENODEV;
 const auto previous=current.load(std::memory_order_acquire);
 if(!networkAllowed)netId=0;
 if(netId){
  KljArtNetwork request{};request.operation=6;
  std::vector<char> bytes;
  for(;;){auto status=klj_art_host(6,&request,sizeof(request));if(status)return static_cast<int>(status);if(request.length<=bytes.size())break;bytes.resize(request.length);request.buffer=reinterpret_cast<uintptr_t>(bytes.data());request.capacity=bytes.size();}
  aidl::android::net::ResolverParamsParcel params;params.netId=static_cast<int32_t>(netId);
  // ConnectivityService's original DnsManager defaults. Zero timeout/retries
  // retain the resolver's own experiment/default handling.
  params.sampleValiditySeconds=1800;params.successThreshold=25;params.minSamples=8;params.maxSamples=64;
  std::istringstream input(bytes.data());std::string line;
  while(std::getline(input,line)){auto separator=line.find('\t');if(separator==std::string::npos)return -EINVAL;auto kind=line.substr(0,separator),value=line.substr(separator+1);if(kind=="server")params.servers.push_back(value);else if(kind=="domain")params.domains.push_back(value);else return -EINVAL;}
  params.transportTypes={(state&4)?IDnsResolver::TRANSPORT_WIFI:(state&8)?IDnsResolver::TRANSPORT_ETHERNET:(state&16)?IDnsResolver::TRANSPORT_CELLULAR:IDnsResolver::TRANSPORT_UNKNOWN};params.meteredNetwork=(state&2)!=0;
  // Only the existing Java connectivity owner selects network identities.
  // Native state retains just its current default; cache ownership stays in
  // the original resolver, including failed configuration rollback.
  if(netId!=previous){int error=result(resolver->createNetworkCache(static_cast<int32_t>(netId)));if(error)return error;}
  int error=result(resolver->setResolverConfiguration(params));
  if(error){if(netId!=previous)(void)resolver->destroyNetworkCache(static_cast<int32_t>(netId));return error;}
 }
 current.store(netId,std::memory_order_release);
 if(previous&&previous!=netId)return result(resolver->destroyNetworkCache(static_cast<int32_t>(previous)));
 return 0;
}
