#include "webkit-binding.h"
#include "webkit-socket.h"
#include <jni.h>
#include <fcntl.h>
#include <sys/un.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace klj::web {
namespace {
struct Pending {bool ready=false;Message kind=Message::reply;Value value;};
struct Transport {
 int socket=-1,wake[2]{-1,-1};
 std::mutex mutex,output;std::condition_variable changed;
 std::map<uint64_t,Pending> pending;std::deque<Frame> events;
 uint64_t next=1;std::string failure;bool closing=false;
 Transport(const char*path,std::u16string token){
  try{socket=::socket(AF_UNIX,SOCK_STREAM,0);if(socket<0)throw std::runtime_error("Browser socket could not be created");sockaddr_un address{};address.sun_family=AF_UNIX;if(strlen(path)>=sizeof(address.sun_path))throw std::runtime_error("Browser socket path is too long");strcpy(address.sun_path,path);
   if(connect(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))||pipe(wake))throw std::runtime_error("Browser window connection failed");
   for(int fd:{socket,wake[0],wake[1]}){fcntl(fd,F_SETFD,FD_CLOEXEC);if(fd!=socket){int flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)throw std::runtime_error("Browser callback pipe could not be configured");}}
   sendFrame(socket,Message::hello,0,0,0,Value(std::move(token)));
  }catch(...){for(int fd:{socket,wake[0],wake[1]})if(fd>=0)close(fd);throw;}
 }
 ~Transport(){for(int fd:{socket,wake[0],wake[1]})if(fd>=0)close(fd);}
 void stop(){
  {std::lock_guard lock(mutex);closing=true;}
  shutdown(socket,SHUT_RDWR);changed.notify_all();
 }
 void notify(){char byte=1;ssize_t n;do{n=write(wake[1],&byte,1);}while(n<0&&errno==EINTR);}
 void read(){try{Frames frames;for(;;){Frame f=frames.wait(socket);std::lock_guard lock(mutex);if(closing)return;
   if(f.header.kind==uint32_t(Message::event)){events.push_back(std::move(f));notify();}
   else if(f.header.kind==uint32_t(Message::reply)||f.header.kind==uint32_t(Message::error)){auto it=pending.find(f.header.id);if(it==pending.end())throw std::runtime_error("Browser reply has no owner");it->second={true,Message(f.header.kind),std::move(f.value)};}
   else throw std::runtime_error("Unexpected browser response");changed.notify_all();
  }}catch(const std::exception&error){std::lock_guard lock(mutex);if(!closing)failure=error.what();notify();changed.notify_all();}}
};
struct Client {
 std::shared_ptr<Transport> state;std::thread reader;std::thread::id owner;
 std::map<uint64_t,std::shared_ptr<Callback>> callbacks;std::map<int64_t,uint64_t> browsers;
 Client(const char*path,std::u16string token):state(std::make_shared<Transport>(path,std::move(token))),owner(std::this_thread::get_id()){
  reader=std::thread([transport=state]{transport->read();});
 }
 void retire(){
  state->stop();if(reader.joinable())reader.join();
  // Explicit Android close releases VM references on its window owner, even
  // while other native requests retain their session lease.
  std::lock_guard lock(state->mutex);callbacks.clear();browsers.clear();
 }
 ~Client(){
  if(reader.joinable()){
   // Global DSO teardown can run under the original loader lock. The worker
   // owns only native transport state and may need that lock to finish. Its
   // shared state closes the descriptors when the worker actually returns.
   state->stop();reader.detach();
  }
 }
 void event(Frame f){
  auto&[socket,wake,mutex,output,changed,pending,events,next,failure,closing]=*state;
  std::shared_ptr<Callback> callback;{std::lock_guard lock(mutex);auto it=callbacks.find(f.header.callback);if(it!=callbacks.end())callback=it->second;}
  Value result;try{if(callback)result=callback->invoke(Event(f.header.operation),f.value.items);if(f.header.id){std::lock_guard lock(output);sendFrame(socket,Message::eventReply,0,f.header.id,0,result);}}
  catch(const std::exception&error){if(!f.header.id)throw;std::lock_guard lock(output);sendFrame(socket,Message::error,0,f.header.id,0,errorValue(error.what()));}
  if(f.header.operation==uint32_t(Event::result)){std::lock_guard lock(mutex);callbacks.erase(f.header.callback);}
 }
 void dispatch(){
  auto&[socket,wake,mutex,output,changed,pending,events,next,failure,closing]=*state;
  if(owner!=std::this_thread::get_id())throw std::runtime_error("Browser callbacks require the Android window thread");
  char bytes[256];while(::read(wake[0],bytes,sizeof(bytes))>0){}
  for(;;){Frame f;{std::lock_guard lock(mutex);if(events.empty()){if(!failure.empty())throw std::runtime_error(failure);return;}f=std::move(events.front());events.pop_front();}event(std::move(f));}
 }
 Value request(Op op,const Args&a,std::shared_ptr<Callback>callback){
  auto&[socket,wake,mutex,output,changed,pending,events,next,failure,closing]=*state;
  uint64_t id,callbackId=0;{std::lock_guard lock(mutex);if(closing||!failure.empty())throw std::runtime_error(failure.empty()?"Browser transport is closed":failure);id=next++;pending.emplace(id,Pending());if(callback){callbackId=next++;callbacks.emplace(callbackId,std::move(callback));}}
  try{
   {std::lock_guard lock(output);sendFrame(socket,Message::request,uint32_t(op),id,callbackId,Value(a));}
   std::unique_lock lock(mutex);for(;;){auto&result=pending.at(id);if(result.ready){auto ready=std::move(result);pending.erase(id);if(ready.kind==Message::error)throw std::runtime_error(errorString(ready.value));if(op==Op::create)browsers[ready.value.number]=callbackId;else if(op==Op::destroy){auto it=browsers.find(a.at(0).number);if(it!=browsers.end()){callbacks.erase(it->second);browsers.erase(it);}}return ready.value;}
    if(closing||!failure.empty())throw std::runtime_error(failure.empty()?"Browser transport is closed":failure);
    if(owner==std::this_thread::get_id()&&!events.empty()){Frame f=std::move(events.front());events.pop_front();lock.unlock();event(std::move(f));lock.lock();continue;}
    changed.wait(lock);
   }
  }catch(...){std::lock_guard lock(mutex);pending.erase(id);if(callbackId)callbacks.erase(callbackId);throw;}
 }
};
std::shared_ptr<Client> client;std::mutex clientMutex;
std::u16string text(JNIEnv*e,jstring s){const auto*p=e->GetStringChars(s,nullptr);if(!p)return {};std::u16string value(reinterpret_cast<const char16_t*>(p),e->GetStringLength(s));e->ReleaseStringChars(s,p);return value;}
void fail(JNIEnv*e,const char*message){if(e->ExceptionCheck())return;jclass type=e->FindClass("java/lang/IllegalStateException");if(type){e->ThrowNew(type,message);e->DeleteLocalRef(type);}}
}
Value request(void*,Op op,const Args&a,std::shared_ptr<Callback>callback){std::shared_ptr<Client>owner;{std::lock_guard lock(clientMutex);owner=client;}if(!owner)throw std::runtime_error("Browser window transport is not configured");return owner->request(op,a,std::move(callback));}
}
extern "C" JNIEXPORT jint JNICALL Java_maclauncher_android_ArtWebKit_openNative(JNIEnv*e,jclass,jstring path,jstring token){try{std::lock_guard lock(klj::web::clientMutex);if(klj::web::client)throw std::runtime_error("Browser transport is already configured");const char*utf=e->GetStringUTFChars(path,nullptr);if(!utf)return -1;std::string socket(utf);e->ReleaseStringUTFChars(path,utf);auto owner=std::make_shared<klj::web::Client>(socket.c_str(),klj::web::text(e,token));int fd=dup(owner->state->wake[0]);if(fd<0)throw std::runtime_error("Browser callback descriptor could not be duplicated");klj::web::client=std::move(owner);return fd;}catch(const std::exception&error){klj::web::fail(e,error.what());return -1;}}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtWebKit_dispatchNative(JNIEnv*e,jclass){try{std::shared_ptr<klj::web::Client>owner;{std::lock_guard lock(klj::web::clientMutex);owner=klj::web::client;}if(owner)owner->dispatch();}catch(const std::exception&error){klj::web::fail(e,error.what());}}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_ArtWebKit_closeNative(JNIEnv*,jclass){std::shared_ptr<klj::web::Client>owner;{std::lock_guard lock(klj::web::clientMutex);owner=std::move(klj::web::client);}if(owner)owner->retire();}
