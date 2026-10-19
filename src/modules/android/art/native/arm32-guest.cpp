#include "arm32-bridge.h"
#include "../../native/arm32/arm32.h"
#include <cstring>
#include <map>
#include <mutex>

// Representation-only binding to the same Memory/Thread/Image implementation.
// JNI and its original references execute on ART's own Bionic stack. The host
// owns CPU execution, mappings, ELF relocation and instruction translation.
namespace maclauncher::arm32 {
namespace {
thread_local Thread *currentThread=nullptr;
struct Execution {
 Thread *previous=currentThread;
 explicit Execution(Thread &thread){currentThread=&thread;}
 ~Execution(){currentThread=previous;}
};
uint64_t request(KljArtArm32 &packet){
 const auto result=klj_art_host(9,&packet,sizeof packet);
 if(result)throw std::runtime_error(packet.error[0]?packet.error:"ARM32 execution transport failed");
 return packet.result;
}
struct Remote {
 uint64_t token;
 explicit Remote(uint64_t token):token(token){}
 ~Remote(){KljArtArm32 packet{};packet.operation=KLJ_ART_ARM32_CLOSE;packet.token=token;klj_art_host(9,&packet,sizeof packet);}
 KljArtArm32 packet(unsigned operation)const{KljArtArm32 value{};value.operation=operation;value.token=token;return value;}
};
using Callback=std::function<uint64_t(KljArtArm32 &)>;
uint64_t dispatch(uint64_t address){
 auto &packet=*reinterpret_cast<KljArtArm32*>(address);
 try{return (*reinterpret_cast<Callback*>(packet.cookie))(packet);}
 catch(const std::exception &failure){std::strncpy(packet.error,failure.what(),sizeof packet.error-1);packet.error[sizeof packet.error-1]=0;return 0;}
}
void continuation(KljArtArm32 &packet,Callback *callback){
 packet.function=reinterpret_cast<uintptr_t>(dispatch);
 packet.cookie=reinterpret_cast<uintptr_t>(callback);
}
uint32_t length(size_t size){if(size>UINT32_MAX)throw Memory::Fault("ARM32 span exceeds its address width");return uint32_t(size);}
struct NativeLease {
 std::shared_ptr<void> storage;
 std::function<void(unsigned)> prepare;
 static uint64_t dispatch(uint64_t address){
  auto &packet=*reinterpret_cast<KljArtArm32*>(address);
  auto *lease=reinterpret_cast<NativeLease*>(packet.cookie);
  if(!packet.buffer){delete lease;return 0;}
  try{lease->prepare(unsigned(packet.buffer-1));}
  catch(const std::exception &failure){std::strncpy(packet.error,failure.what(),sizeof packet.error-1);packet.error[sizeof packet.error-1]=0;}
  return 0;
 }
};
}
struct Memory::Impl:Remote {
 std::mutex mutex;
 std::map<uint32_t,std::unique_ptr<Callback>> callbacks;
 explicit Impl(uint64_t token):Remote(token){}
};
Memory::Memory(){KljArtArm32 packet{};packet.operation=KLJ_ART_ARM32_MEMORY;impl=std::make_unique<Impl>(request(packet));}
Memory::~Memory()=default;
void Memory::map(uint32_t address,uint32_t size,unsigned permission){auto p=impl->packet(KLJ_ART_ARM32_MAP);p.address=address;p.length=size;p.permission=permission;request(p);}
uint32_t Memory::allocate(uint32_t address,uint32_t size,unsigned permission){auto p=impl->packet(KLJ_ART_ARM32_ALLOCATE);p.address=address;p.length=size;p.permission=permission;return uint32_t(request(p));}
void Memory::unmap(uint32_t address){unmap(address,0);}
void Memory::unmap(uint32_t address,uint32_t size){auto p=impl->packet(KLJ_ART_ARM32_UNMAP);p.address=address;p.length=size;request(p);}
void Memory::protect(uint32_t address,uint32_t size,unsigned permission){auto p=impl->packet(KLJ_ART_ARM32_PROTECT);p.address=address;p.length=size;p.permission=permission;request(p);}
void *Memory::translate(uint32_t address,size_t size,unsigned permission)const{auto p=impl->packet(KLJ_ART_ARM32_TRANSLATE);p.address=address;p.length=length(size);p.permission=permission;return reinterpret_cast<void*>(request(p));}
void *Memory::loadAddress(uint32_t address,size_t size)const{auto p=impl->packet(KLJ_ART_ARM32_LOAD_ADDRESS);p.address=address;p.length=length(size);return reinterpret_cast<void*>(request(p));}
uint32_t Memory::borrow(void *bytes,uint32_t size,unsigned permission,std::string_view origin){auto p=impl->packet(KLJ_ART_ARM32_BORROW);p.buffer=reinterpret_cast<uintptr_t>(bytes);p.length=size;p.permission=permission;p.function=reinterpret_cast<uintptr_t>(origin.data());p.count=length(origin.size());return uint32_t(request(p));}
uint32_t Memory::addressOf(const void *bytes)const{auto p=impl->packet(KLJ_ART_ARM32_ADDRESS);p.buffer=reinterpret_cast<uintptr_t>(bytes);return uint32_t(request(p));}
uint32_t Memory::mapNative(void *bytes,uint32_t size,unsigned permission,std::shared_ptr<void> owner,uint32_t preferred,bool fixed,bool noReplace,std::function<void(unsigned)> prepare,bool shared){
 auto lease=std::make_unique<NativeLease>(NativeLease{std::move(owner),std::move(prepare)});
 auto p=impl->packet(KLJ_ART_ARM32_MAP_NATIVE);p.buffer=reinterpret_cast<uintptr_t>(bytes);p.length=size;p.permission=permission;p.address=preferred;
 p.reserved=(fixed?1u:0u)|(noReplace?2u:0u)|(lease->prepare?4u:0u)|(shared?8u:0u);
 p.function=reinterpret_cast<uintptr_t>(NativeLease::dispatch);p.cookie=reinterpret_cast<uintptr_t>(lease.get());
 const auto error=klj_art_host(9,&p,sizeof p);
 if(p.count)lease.release();
 if(error)throw Memory::Fault(p.error[0]?p.error:"Native memory mapping failed");
 return uint32_t(p.result);
}
void Memory::retireNative(const void *bytes,size_t size){auto p=impl->packet(KLJ_ART_ARM32_RETIRE_NATIVE);p.buffer=reinterpret_cast<uintptr_t>(bytes);p.budget=size;request(p);}
void Memory::invalidate(uint32_t address,uint32_t size){auto p=impl->packet(KLJ_ART_ARM32_INVALIDATE);p.address=address;p.length=size;request(p);}
std::string Memory::mappingDescription(uint32_t address)const{
 auto p=impl->packet(KLJ_ART_ARM32_MAPPING_DESCRIPTION);p.address=address;
 std::string value(size_t(request(p)),0);p.buffer=reinterpret_cast<uintptr_t>(value.data());p.length=length(value.size()+1);request(p);return value;
}
std::vector<Memory::Mapping> Memory::mappings()const{
 auto p=impl->packet(KLJ_ART_ARM32_MAPPINGS);
 std::vector<Mapping> value;
 for(;;){
  p.buffer=reinterpret_cast<uintptr_t>(value.data());p.count=length(value.size());
  const auto count=request(p);
  if(count<=value.size()){value.resize(size_t(count));return value;}
  if(count>SIZE_MAX/sizeof(Mapping)||count>UINT32_MAX)throw Memory::Fault("ARM32 mapping snapshot exceeds its address width");
  value.resize(size_t(count));
 }
}
std::string Memory::string(uint32_t address)const{
 auto p=impl->packet(KLJ_ART_ARM32_STRING);p.address=address;
 std::string value(size_t(request(p)),0);p.buffer=reinterpret_cast<uintptr_t>(value.data());p.length=length(value.size()+1);request(p);return value;
}
uint32_t Memory::import(HostCall function){
 auto callback=std::make_unique<Callback>([function=std::move(function)](KljArtArm32 &){
  if(!currentThread)throw std::runtime_error("ARM32 callback has no execution context");
  return function(*currentThread);
 });
 auto p=impl->packet(KLJ_ART_ARM32_GATE);continuation(p,callback.get());const auto address=uint32_t(request(p));
 try{std::lock_guard lock(impl->mutex);impl->callbacks.emplace(address,std::move(callback));}
 catch(...){p.operation=KLJ_ART_ARM32_RETIRE_GATE;p.address=address;request(p);throw;}
 return address;
}
void Memory::retireImport(uint32_t address){auto p=impl->packet(KLJ_ART_ARM32_RETIRE_GATE);p.address=address;request(p);std::lock_guard lock(impl->mutex);impl->callbacks.erase(address);}

struct Thread::Impl:Remote {
 Memory &memory;
 std::unique_ptr<Callback> syscall;
 Impl(uint64_t token,Memory &memory):Remote(token),memory(memory){}
};
Thread::Thread(Memory &memory,uint32_t stackBase,uint32_t stackSize){
 auto p=memory.impl->packet(KLJ_ART_ARM32_THREAD);p.parent=p.token;p.address=stackBase;p.length=stackSize;
 impl=std::make_unique<Impl>(request(p),memory);
}
Thread::~Thread()=default;
uint32_t Thread::import(HostCall call){return impl->memory.import(std::move(call));}
uint64_t Thread::call(uint32_t address,std::span<const Value> arguments,uint64_t budget){
 static_assert(sizeof(Value)==sizeof(KljArtArm32Value));
 auto p=impl->packet(KLJ_ART_ARM32_CALL);p.address=address;p.buffer=reinterpret_cast<uintptr_t>(arguments.data());p.count=length(arguments.size());p.budget=budget;
 Execution execution(*this);return request(p);
}
uint32_t Thread::argument(unsigned index)const{auto p=impl->packet(KLJ_ART_ARM32_ARGUMENT);p.address=index;return uint32_t(request(p));}
void Thread::arguments(unsigned first,std::span<uint32_t> words)const{
 if(words.empty())return;
 auto p=impl->packet(KLJ_ART_ARM32_ARGUMENTS);p.address=first;
 p.buffer=reinterpret_cast<uintptr_t>(words.data());p.count=length(words.size());request(p);
}
uint32_t Thread::syscallArgument(unsigned index)const{auto p=impl->packet(KLJ_ART_ARM32_SYSCALL_ARGUMENT);p.address=index;return uint32_t(request(p));}
uint32_t Thread::returnAddress()const{auto p=impl->packet(KLJ_ART_ARM32_RETURN_ADDRESS);return uint32_t(request(p));}
uint32_t Thread::tls()const{auto p=impl->packet(KLJ_ART_ARM32_TLS);return uint32_t(request(p));}
void Thread::setTls(uint32_t address){auto p=impl->packet(KLJ_ART_ARM32_SET_TLS);p.address=address;request(p);}
Memory &Thread::memory(){return impl->memory;}
void Thread::setSyscall(std::function<int64_t(Thread &,uint32_t)> function){
 std::unique_ptr<Callback> next;
 auto p=impl->packet(KLJ_ART_ARM32_SET_SYSCALL);
 if(function){next=std::make_unique<Callback>([this,function=std::move(function)](KljArtArm32 &input){return uint64_t(function(*this,uint32_t(input.buffer)));});continuation(p,next.get());}
 request(p);impl->syscall=std::move(next);
}

struct Image::Impl:Remote {
 std::unique_ptr<Callback> resolver;
 Impl(uint64_t token,std::unique_ptr<Callback> resolver):Remote(token),resolver(std::move(resolver)){}
};
Image::Layout Image::inspect(std::span<const uint8_t> bytes){
 KljArtArm32 packet{};packet.operation=KLJ_ART_ARM32_IMAGE_INSPECT;packet.buffer=reinterpret_cast<uintptr_t>(bytes.data());packet.length=length(bytes.size());
 Layout result{};result.span=uint32_t(request(packet));std::vector<char> names(packet.count);
 if(!names.empty()){
  packet.function=reinterpret_cast<uintptr_t>(names.data());request(packet);size_t offset=0;
  for(uint32_t i=0;i<packet.reserved;i++){
   if(offset>=names.size())throw std::runtime_error("Truncated ARM32 dependency metadata");
   auto *end=static_cast<const char*>(std::memchr(names.data()+offset,0,names.size()-offset));
   if(!end)throw std::runtime_error("Unterminated ARM32 dependency metadata");
   result.needed.emplace_back(names.data()+offset,size_t(end-(names.data()+offset)));offset=size_t(end-names.data())+1;
  }
 }
 return result;
}
Image::Image(Memory &memory,std::span<const uint8_t> bytes,uint32_t bias,Resolve resolver){
 auto resolve=std::make_unique<Callback>([resolver=std::move(resolver)](KljArtArm32 &p){return resolver(reinterpret_cast<const char*>(p.buffer));});
 auto p=memory.impl->packet(KLJ_ART_ARM32_IMAGE);p.buffer=reinterpret_cast<uintptr_t>(bytes.data());p.length=length(bytes.size());p.address=bias;continuation(p,resolve.get());
 const auto token=request(p);impl=std::make_unique<Impl>(token,std::move(resolve));
}
Image::Image(Memory &memory,std::span<const uint8_t> bytes,uint32_t bias){
 auto p=memory.impl->packet(KLJ_ART_ARM32_IMAGE);p.buffer=reinterpret_cast<uintptr_t>(bytes.data());p.length=length(bytes.size());p.address=bias;p.reserved=1;
 impl=std::make_unique<Impl>(request(p),nullptr);
}
void Image::relocate(Resolve resolver){
 auto resolve=std::make_unique<Callback>([resolver=std::move(resolver)](KljArtArm32 &p){return resolver(reinterpret_cast<const char*>(p.buffer));});
 auto p=impl->packet(KLJ_ART_ARM32_IMAGE_RELOCATE);continuation(p,resolve.get());request(p);
 if(!impl->resolver)impl->resolver=std::move(resolve);
}
Image::~Image()=default;
void Image::initialize(Thread &thread){auto p=impl->packet(KLJ_ART_ARM32_IMAGE_INITIALIZE);p.parent=thread.impl->token;Execution execution(thread);request(p);}
uint32_t Image::symbol(std::string_view name)const{const std::string terminated(name);auto p=impl->packet(KLJ_ART_ARM32_IMAGE_SYMBOL);p.buffer=reinterpret_cast<uintptr_t>(terminated.c_str());return uint32_t(request(p));}
uint32_t Image::base()const{auto p=impl->packet(KLJ_ART_ARM32_IMAGE_BASE);return uint32_t(request(p));}
uint32_t Image::span()const{auto p=impl->packet(KLJ_ART_ARM32_IMAGE_SPAN);return uint32_t(request(p));}
std::string Image::runpath()const{auto p=impl->packet(KLJ_ART_ARM32_IMAGE_RUNPATH);std::string text(size_t(request(p)),0);p.buffer=reinterpret_cast<uintptr_t>(text.data());p.length=length(text.size()+1);request(p);return text;}
const void *Image::programHeaders(unsigned *count)const{auto p=impl->packet(KLJ_ART_ARM32_IMAGE_HEADERS);const auto address=request(p);if(count)*count=p.count;return reinterpret_cast<const void*>(address);}
} // namespace maclauncher::arm32
