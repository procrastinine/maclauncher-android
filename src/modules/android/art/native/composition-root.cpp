// Native window authority. Original SurfaceFlinger owns the layer; the app gets
// an original SurfaceControl capability, never a host-native pointer.
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <android/os/IServiceManager.h>
#include <android/os/BnServiceCallback.h>
#include <binder/Parcel.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <ui/DisplayMode.h>
#include <ui/DynamicDisplayInfo.h>
#include <android/gui/DisplayModeSpecs.h>
#include <algorithm>
#include <media/AudioSystem.h>
#include <audiomanager/IAudioManager.h>
#include <cstdio>
#include <android/surface_control.h>
#include <android/binder_libbinder.h>
#include <binder_rpc_unstable.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <thread>
#include <chrono>
#include "resolver-guest.h"
#include "bridge.h"
#include "composition-authority.hpp"
#include <arpa/inet.h>
using namespace android;
extern "C" int klj_art_rpc_peer(void*,int,unsigned,const char*,const char*,const char*);
struct PlatformEndpoint {const char*path;pid_t pid;};
static int platformSocket(void*opaque){
 const auto&endpoint=*static_cast<PlatformEndpoint*>(opaque);sockaddr_un address{};address.sun_family=AF_UNIX;
 if(strlen(endpoint.path)>=sizeof(address.sun_path)){errno=ENAMETOOLONG;return -1;}strcpy(address.sun_path,endpoint.path);
 int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
 if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))){int saved=errno;close(fd);errno=saved;return -1;}
 ucred peer{};socklen_t size=sizeof(peer);if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)||size!=sizeof(peer)||peer.pid!=endpoint.pid){close(fd);errno=EACCES;return -1;}return fd;
}
// Preserve the complete original Binder interface and Parcel, forwarding only
// at the native-kernel/RPC transport boundary. Policy remains in the enrolled
// profile's existing Java permission/package/app-operation owners.
class PlatformService final:public BBinder {
 const sp<IBinder> target;
 public:explicit PlatformService(sp<IBinder> value):target(std::move(value)){}
 const String16&getInterfaceDescriptor()const override{return target->getInterfaceDescriptor();}
 status_t onTransact(uint32_t code,const Parcel&data,Parcel*reply,uint32_t flags)override{
  if(code>=FIRST_CALL_TRANSACTION&&code<=LAST_CALL_TRANSACTION){
   const auto position=data.dataPosition();const bool valid=data.checkInterface(this);data.setDataPosition(position);if(!valid)return BAD_TYPE;
  }
  return target->transact(code,data,reply,flags);
 }
};
// Transport leases, not a second service catalog. Enumerate the authenticated
// directory and subscribe using its original callback API. Native registrations
// take precedence, including native services published after this bridge starts.
class PlatformDirectory final:public os::BnServiceCallback {
 const sp<os::IServiceManager> directory;
 std::mutex lock;std::map<std::string,sp<PlatformService>> leases;
 std::set<std::string> subscribed;
 public:explicit PlatformDirectory(sp<os::IServiceManager> value):directory(std::move(value)){}
 binder::Status onRegistration(const std::string&name,const sp<IBinder>&binder)override{
  if(!binder)return binder::Status::ok();
  std::lock_guard<std::mutex> guard(lock);
  auto local=defaultServiceManager()->checkService(String16(name.c_str()));
  auto found=leases.find(name);
  if(local&&(found==leases.end()||local!=found->second))return binder::Status::ok();
  auto relay=sp<PlatformService>::make(binder);
  const auto status=defaultServiceManager()->addService(String16(name.c_str()),relay,false);
  if(status!=OK)return binder::Status::fromStatusT(status);
  leases[name]=std::move(relay);fprintf(stderr,"Canonical Binder transported: %s\n",name.c_str());
  return binder::Status::ok();
 }
 bool discover(){
  std::vector<std::string> names;
  auto status=directory->listServices(IServiceManager::DUMP_FLAG_PRIORITY_ALL,&names);
  if(!status.isOk()){fprintf(stderr,"Canonical service discovery: %s\n",status.toString8().c_str());return false;}
  for(const auto&name:names){
   if(subscribed.count(name))continue;
   // Registration immediately delivers the current original Binder and later
   // replacements. Do not hold a lock across that synchronous RPC callback.
   auto registered=directory->registerForNotifications(name,sp<os::IServiceCallback>::fromExisting(this));
   if(registered.isOk())subscribed.insert(name);
   else fprintf(stderr,"Canonical service subscription %s: %s\n",name.c_str(),registered.toString8().c_str());
  }
  return true;
 }
 void start(){
  // IServiceManager callbacks are per name. Discovery of newly published names
  // stays off transaction/render paths and uses the same authoritative list.
  auto self=sp<PlatformDirectory>::fromExisting(this);
  std::thread([self]{for(;;){std::this_thread::sleep_for(std::chrono::seconds(1));if(!self->discover())return;}}).detach();
 }
};
// Publish the original native policy object, including its original permission
// controller, into the authenticated system directory. Binder owns the lease;
// policy/service state remains in AudioPolicyService and the shared Java owner.
class NativePublication final:public IServiceManager::LocalRegistrationCallback,public IBinder::DeathRecipient {
 const sp<os::IServiceManager> directory;std::mutex lock;sp<IBinder> current;
 public:explicit NativePublication(sp<os::IServiceManager> value):directory(std::move(value)){}
 status_t publish(const sp<IBinder>& binder){
  std::lock_guard<std::mutex> guard(lock);if(current==binder)return OK;
  auto status=directory->addService("media.audio_policy",binder,false,IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT);
  if(!status.isOk()){fprintf(stderr,"Native audio policy publication: %s\n",status.toString8().c_str());return status.transactionError()==OK?UNKNOWN_ERROR:status.transactionError();}
  // Registration queues the original provider's complete initial replay in
  // the shared system owner. Its original barrier drains that same owner;
  // a published policy Binder alone does not mean all permissions arrived.
  os::Service value;status=directory->checkService2("audio",&value);
  if(!status.isOk())return status.transactionError()==OK?UNKNOWN_ERROR:status.transactionError();
  if(value.getTag()!=os::Service::Tag::serviceWithMetadata)return BAD_TYPE;
  auto audio=interface_cast<IAudioManager>(value.get<os::Service::Tag::serviceWithMetadata>().service);
  if(!audio)return NAME_NOT_FOUND;
  auto ready=audio->permissionUpdateBarrier();if(ready!=OK)return ready;
  current=binder;auto death=binder->linkToDeath(sp<IBinder::DeathRecipient>::fromExisting(this));if(death!=OK)fprintf(stderr,"Native audio policy death registration: %d\n",death);
  fprintf(stderr,"Original native audio policy and complete permission state ready\n");return OK;
 }
 void onServiceRegistration(const String16&,const sp<IBinder>& binder)override{
  auto status=publish(binder);if(status!=OK)fprintf(stderr,"Native audio permission initialization: %d\n",status);
 }
 void binderDied(const wp<IBinder>& who)override{
  sp<IBinder> retired;{std::lock_guard<std::mutex> guard(lock);if(who!=current)return;retired=std::move(current);}
  auto status=directory->tryUnregisterService("media.audio_policy",retired);
  if(!status.isOk())fprintf(stderr,"Native audio policy retirement: %s\n",status.toString8().c_str());
 }
};
class WindowRoot final:public BBinder {
 const uid_t app;const sp<SurfaceControl> surface;
 const sp<IBinder> display;const int64_t displayId;
 const sp<NativePublication> audioPublication;
 std::mutex publication;sp<IBinder> activity,audio,appops,checker,connectivity;
 public:WindowRoot(uid_t uid,sp<SurfaceControl> value,sp<IBinder> output,int64_t id,sp<NativePublication> audioOwner):app(uid),surface(std::move(value)),display(std::move(output)),displayId(id),audioPublication(std::move(audioOwner)){}
 status_t onTransact(uint32_t code,const Parcel&data,Parcel*reply,uint32_t flags)override{
  if(code<FIRST_CALL_TRANSACTION||code>FIRST_CALL_TRANSACTION+7)return BBinder::onTransact(code,data,reply,flags);
  if(IPCThreadState::self()->getCallingUid()!=app)return PERMISSION_DENIED;
  if(!data.checkInterface(this))return BAD_TYPE;
  if(code==FIRST_CALL_TRANSACTION+7){
   int32_t landscape;if(data.readInt32(&landscape)!=OK||data.dataAvail()||(landscape!=0&&landscape!=1))return BAD_VALUE;
   struct Identity {int64_t token=IPCThreadState::self()->clearCallingIdentity();~Identity(){IPCThreadState::self()->restoreCallingIdentity(token);}} identity;
   ui::DynamicDisplayInfo info;auto status=SurfaceComposerClient::getDynamicDisplayInfoFromId(displayId,&info);if(status!=OK)return status;
   auto mode=std::find_if(info.supportedDisplayModes.begin(),info.supportedDisplayModes.end(),[&](const auto&value){return (value.resolution.getWidth()>=value.resolution.getHeight())==bool(landscape);});
   if(mode==info.supportedDisplayModes.end())return NAME_NOT_FOUND;
   gui::DisplayModeSpecs specs;status=SurfaceComposerClient::getDesiredDisplayModeSpecs(display,&specs);if(status!=OK)return status;
   specs.defaultMode=mode->id;specs.allowGroupSwitching=true;
   status=SurfaceComposerClient::setDesiredDisplayModeSpecs(display,specs);if(status!=OK)return status;
   Rect bounds(mode->resolution.getWidth(),mode->resolution.getHeight());
   SurfaceComposerClient::Transaction transaction;transaction.setDisplayProjection(display,ui::ROTATION_0,bounds,bounds);return transaction.apply(true);
  }
  if(code==FIRST_CALL_TRANSACTION+6){
   int32_t netId,state;if(data.readInt32(&netId)!=OK||data.readInt32(&state)!=OK||data.dataAvail()||netId<0)return BAD_VALUE;
   struct Identity {int64_t token=IPCThreadState::self()->clearCallingIdentity();~Identity(){IPCThreadState::self()->restoreCallingIdentity(token);}} identity;
   return klj_art_resolver_network(static_cast<uint32_t>(netId),state);
  }
  if(code==FIRST_CALL_TRANSACTION+5){
   String16 name=data.readString16();const bool isOps=name==String16("appops");
   const bool isNetwork=name==String16("connectivity");if(!isOps&&!isNetwork&&name!=String16("permission_checker"))return BAD_VALUE;
   auto endpoint=data.readStrongBinder();auto& retained=isOps?appops:isNetwork?connectivity:checker;
   if(!endpoint||data.dataAvail()||endpoint->getInterfaceDescriptor()!=String16(isOps?"com.android.internal.app.IAppOpsService":isNetwork?"android.net.IConnectivityManager":"android.permission.IPermissionChecker"))return BAD_TYPE;
   std::lock_guard<std::mutex> guard(publication);
   if(retained&&retained->isBinderAlive())return retained==endpoint?OK:ALREADY_EXISTS;
   auto status=defaultServiceManager()->addService(name,endpoint,false);if(status==OK)retained=std::move(endpoint);return status;
  }
  if(code==FIRST_CALL_TRANSACTION+2||code==FIRST_CALL_TRANSACTION+3){
   // The existing Java audio owner supplies policy; this enrolled authority
   // performs only the corresponding original native settings operations.
   // There is no copied gain table or general privileged transaction relay.
   struct Identity {int64_t token=IPCThreadState::self()->clearCallingIdentity();~Identity(){IPCThreadState::self()->restoreCallingIdentity(token);}} identity;
   if(code==FIRST_CALL_TRANSACTION+3){
    int32_t stream,index;if(data.readInt32(&stream)!=OK||data.readInt32(&index)!=OK||data.dataAvail())return BAD_VALUE;
    return AudioSystem::setStreamVolumeIndex(static_cast<audio_stream_type_t>(stream),index,index==0,AUDIO_DEVICE_OUT_DEFAULT);
   }
   int32_t count;if(data.readInt32(&count)!=OK||count!=AUDIO_STREAM_PUBLIC_CNT)return BAD_VALUE;
   struct Range {int32_t min,max,index;};Range ranges[AUDIO_STREAM_PUBLIC_CNT];
   for(auto&range:ranges)if(data.readInt32(&range.min)!=OK||data.readInt32(&range.max)!=OK||data.readInt32(&range.index)!=OK||range.min<0||range.max<range.min||range.index<range.min||range.index>range.max)return BAD_VALUE;
   if(data.dataAvail())return BAD_VALUE;
   auto policy=defaultServiceManager()->waitForService(String16("media.audio_policy"));if(!policy)return DEAD_OBJECT;
   auto permissionsReady=audioPublication->publish(policy);if(permissionsReady!=OK)return permissionsReady;
   for(int32_t stream=0;stream<count;++stream){const auto&range=ranges[stream];auto type=static_cast<audio_stream_type_t>(stream);
    auto status=AudioSystem::initStreamVolume(type,range.min,range.max);if(status!=OK)return status;
    status=AudioSystem::setStreamVolumeIndex(type,range.index,range.index==0,AUDIO_DEVICE_OUT_DEFAULT);if(status!=OK)return status;
   }
   auto ready=AudioSystem::systemReady();if(ready!=OK)return ready;
   fprintf(stderr,"Original AudioSystem initialized %d streams and Java service callbacks from the shared audio owner\n",count);return OK;
  }
  if(code==FIRST_CALL_TRANSACTION+1||code==FIRST_CALL_TRANSACTION+4){
   // This existing per-app authority publishes the app's complete original
   // IActivityManager endpoint. Binder retains its real app process and UID;
   // callbacks and observer registrations never cross a copied Parcel map.
   const bool isActivity=code==FIRST_CALL_TRANSACTION+1;auto& retained=isActivity?activity:audio;
   auto endpoint=data.readStrongBinder();
   if(!endpoint||endpoint->getInterfaceDescriptor()!=String16(isActivity?"android.app.IActivityManager":"android.media.IAudioService"))return BAD_TYPE;
   std::lock_guard<std::mutex> guard(publication);
   if(retained&&retained->isBinderAlive())return retained==endpoint?OK:ALREADY_EXISTS;
   auto status=defaultServiceManager()->addService(String16(isActivity?"activity":"audio"),endpoint,false);
   if(status==OK)retained=std::move(endpoint);
   return status;
  }
  return surface->writeToParcel(*reply);
 }
 const String16&getInterfaceDescriptor()const override{static const String16 name("maclauncher.android.WindowOutput");return name;}
};
int main(int argc,char**argv){
 if(argc==2&&strcmp(argv[1],"--service-directory")==0){
  // Keep absence distinct from a failed Binder transaction. This original
  // directory call runs before SurfaceFlinger is started by the launcher.
  auto context=IInterface::asBinder(defaultServiceManager());if(!context)return 15;
  auto directory=interface_cast<os::IServiceManager>(context);if(!directory)return 15;
  os::Service service;auto status=directory->checkService2("SurfaceFlinger",&service);
  if(!status.isOk()){fprintf(stderr,"Android service directory: %s\n",status.toString8().c_str());return 15;}
  return 0;
 }
 if(argc==3&&strcmp(argv[1],"--wait-service")==0){
  ProcessState::self()->startThreadPool();
  if(!defaultServiceManager()->waitForService(String16(argv[2]))){fprintf(stderr,"Android service unavailable: %s\n",argv[2]);return 15;}
  return 0;
 }
 const bool deferred=argc==5&&strcmp(argv[1],"--platform-stdin")==0;
 if(!deferred&&argc!=10)return 2;const uid_t app=static_cast<uid_t>(strtoul(argv[deferred?2:1],nullptr,10));if(app<10000)return 2;
 const auto physical=klj_art_host(11,nullptr,0);if(physical<=0||physical>INT32_MAX)return 7;
 char message[128];int length=snprintf(message,sizeof(message),"{\"op\":\"identity\",\"pid\":%d,\"hostPid\":%d}",getpid(),static_cast<int>(physical));
 if(length<=0||static_cast<size_t>(length)>=sizeof(message))return 7;
 uint32_t prefix=htonl(static_cast<uint32_t>(length));
 for(const auto&part:{std::pair<const void*,size_t>{&prefix,sizeof(prefix)},{message,static_cast<size_t>(length)}}){
  const auto*bytes=static_cast<const char*>(part.first);size_t done=0;
  while(done<part.second){ssize_t count=write(5,bytes+done,part.second-done);if(count<0&&errno==EINTR)continue;if(count<=0)return 7;done+=static_cast<size_t>(count);}
 }
 // The owning launcher enrolls this actual process before releasing stdin.
 char enrolled;if(!klj_composition::readBytes(STDIN_FILENO,&enrolled,1)||enrolled!='1')return 7;
 std::array<std::string,6> authority;
 if(deferred&&!klj_composition::readAuthority(STDIN_FILENO,authority))return 7;
 const char *socketPath=deferred?authority[0].c_str():argv[2],*peerPid=deferred?authority[1].c_str():argv[3],*peerUid=deferred?authority[2].c_str():argv[4];
 const char *peerContext=deferred?authority[3].c_str():argv[5],*peerPackage=deferred?authority[4].c_str():argv[6],*peerProcess=deferred?authority[5].c_str():argv[7];
 if(!klj_art_resolver_prepare())return 14;
 PlatformEndpoint endpoint{socketPath,static_cast<pid_t>(strtol(peerPid,nullptr,10))};
 ARpcSession*rpc=ARpcSession_new();if(!rpc)return 8;
 if(klj_art_rpc_peer(rpc,endpoint.pid,static_cast<unsigned>(strtoul(peerUid,nullptr,10)),peerContext,peerPackage,peerProcess))return 8;
 ARpcSession_setFileDescriptorTransportMode(rpc,ARpcSession_FileDescriptorTransportMode::Unix);
 ARpcSession_setMaxIncomingThreads(rpc,1);
 AIBinder*remote=ARpcSession_setupPreconnectedClient(rpc,platformSocket,&endpoint,nullptr);if(!remote)return 9;
 auto directory=interface_cast<os::IServiceManager>(AIBinder_toPlatformBinder(remote));AIBinder_decStrong(remote);
 ProcessState::self()->startThreadPool();
 auto platform=sp<PlatformDirectory>::make(directory);
 if(!platform->discover())return 10;
 platform->start();
 if(!klj_art_resolver_start(strcmp(argv[deferred?3:8],"1")==0,strcmp(argv[deferred?4:9],"1")==0)){fprintf(stderr,"Original DNS resolver could not start: %s\n",strerror(errno));return 14;}
 auto audioPublication=sp<NativePublication>::make(directory);
 if(defaultServiceManager()->registerForNotifications(String16("media.audio_policy"),audioPublication)!=OK)return 12;
 auto client=SurfaceComposerClient::getDefault();if(client->initCheck()!=OK)return 3;
 // The existing physical presenter supplies the HWC display. Bind its actual
 // mode to the original app layer stack, as DisplayManager normally does.
 // No dimensions or display identities are invented by the guest bootstrap.
 auto displays=SurfaceComposerClient::getPhysicalDisplayIds();if(displays.empty())return 13;
 auto display=SurfaceComposerClient::getPhysicalDisplayToken(displays.front());if(!display)return 13;
 ui::DisplayMode mode;if(SurfaceComposerClient::getActiveDisplayMode(display,&mode)!=OK)return 13;
 Rect bounds(mode.resolution.getWidth(),mode.resolution.getHeight());
 auto surface=client->createSurface(String8("MacLauncher Android output"),0,0,PIXEL_FORMAT_RGBA_8888,gui::ISurfaceComposerClient::eFXSurfaceContainer);
 if(!surface)return 4;auto native=ASurfaceTransaction_create();auto*transaction=reinterpret_cast<SurfaceComposerClient::Transaction*>(native);transaction->setDisplayLayerStack(display,ui::DEFAULT_LAYER_STACK);transaction->setDisplayProjection(display,ui::ROTATION_0,bounds,bounds);transaction->setLayerStack(surface,ui::DEFAULT_LAYER_STACK).setLayer(surface,1).show(surface);auto applied=transaction->apply(true);ASurfaceTransaction_delete(native);if(applied!=OK)return 5;
 auto root=sp<WindowRoot>::make(app,surface,display,displays.front().value,audioPublication);status_t status=defaultServiceManager()->addService(String16("maclauncher.art.window"),root,false);if(status!=OK){fprintf(stderr,"Original window root registration %d\n",status);return 6;}
 fprintf(stderr,"Original SurfaceControl window root ready for uid %u\n",app);IPCThreadState::self()->joinThreadPool();return 0;
}
