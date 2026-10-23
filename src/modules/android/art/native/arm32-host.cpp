#include "arm32-bridge.h"
#include "host-packets.h"
#include "../../native/arm32/arm32.h"
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace {
using namespace maclauncher::arm32;
struct Owner {
 std::shared_ptr<Memory> memory;
 std::unique_ptr<Thread> thread;
 std::unique_ptr<Image> image;
 std::thread::id creator;
};
std::mutex ownersMutex;
std::map<uint64_t,std::shared_ptr<Owner>> owners;
uint64_t nextOwner=1;
std::shared_ptr<Owner> owner(uint64_t token){
 std::lock_guard lock(ownersMutex);
 auto found=owners.find(token);
 if(found==owners.end())throw std::runtime_error("Retired ARM32 execution owner");
 return found->second;
}
uint64_t publish(std::shared_ptr<Owner> value){
 std::lock_guard lock(ownersMutex);
 if(!nextOwner)throw std::runtime_error("ARM32 execution identity exhausted");
 const auto token=nextOwner++;owners.emplace(token,std::move(value));return token;
}
uint64_t continuation(KljArtCallback callback,uint64_t function,uint64_t cookie,
                      uint64_t input=0){
 if(!callback||!function)throw std::runtime_error("Missing ART guest continuation");
 KljArtArm32 packet{};packet.operation=KLJ_ART_ARM32_CALLBACK;
 packet.cookie=cookie;packet.buffer=input;
 const auto result=callback(function,reinterpret_cast<uintptr_t>(&packet));
 if(packet.error[0])throw std::runtime_error(packet.error);
 return result;
}
}

extern "C" int64_t maclauncher_art_arm32(uint64_t address,uint64_t size,KljArtCallback callback){
 if(!address||size!=sizeof(KljArtArm32))return -22;
 int64_t packet_error=klj_art_resolve_packet(address,size,KLJ_ART_PACKET_ALIGN(KljArtArm32),KLJ_ART_PACKET_READ_WRITE,&address);if(packet_error)return packet_error;
 auto &packet=*reinterpret_cast<KljArtArm32*>(address);
 packet.error[0]=0;
 try{
  if(packet.operation==KLJ_ART_ARM32_IMAGE_INSPECT){
   auto layout=Image::inspect(std::span(reinterpret_cast<const uint8_t*>(packet.buffer),packet.length));
   size_t size=0;for(auto &name:layout.needed)size+=name.size()+1;
   if(size>UINT32_MAX)throw std::runtime_error("ARM32 dependency metadata exceeds its address width");
   if(packet.function){
    if(packet.count<size)throw std::runtime_error("ARM32 dependency metadata buffer is too small");
    auto *output=reinterpret_cast<char*>(packet.function);for(auto &name:layout.needed){std::memcpy(output,name.c_str(),name.size()+1);output+=name.size()+1;}
   }
   packet.count=uint32_t(size);packet.reserved=uint32_t(layout.needed.size());packet.result=layout.span;return 0;
  }
  if(packet.operation==KLJ_ART_ARM32_MEMORY){
   auto value=std::make_shared<Owner>();value->memory=std::make_shared<Memory>();
   packet.result=publish(std::move(value));return 0;
  }
  if(packet.operation==KLJ_ART_ARM32_THREAD){
   auto value=std::make_shared<Owner>();value->memory=owner(packet.parent)->memory;
   value->thread=packet.length?std::make_unique<Thread>(*value->memory,packet.address,packet.length):std::make_unique<Thread>(*value->memory);value->creator=std::this_thread::get_id();
   packet.result=publish(std::move(value));return 0;
  }
  if(packet.operation==KLJ_ART_ARM32_CLOSE){
   std::shared_ptr<Owner> retired;
   {std::lock_guard lock(ownersMutex);auto found=owners.find(packet.token);
    if(found==owners.end())return -22;retired=std::move(found->second);owners.erase(found);}
   return 0;
  }
  auto value=owner(packet.token);
  switch(packet.operation){
  case KLJ_ART_ARM32_MAP:
   value->memory->map(packet.address,packet.length,Memory::Read|Memory::Write);
   try{if(packet.buffer)std::memcpy(value->memory->loadAddress(packet.address,packet.length),reinterpret_cast<const void*>(packet.buffer),packet.length);
    value->memory->protect(packet.address,packet.length,packet.permission);
   }catch(...){value->memory->unmap(packet.address,packet.length);throw;}
   break;
  case KLJ_ART_ARM32_UNMAP:
   if(packet.length)value->memory->unmap(packet.address,packet.length);
   else value->memory->unmap(packet.address);break;
  case KLJ_ART_ARM32_ALLOCATE:packet.result=value->memory->allocate(packet.address,packet.length,packet.permission);break;
  case KLJ_ART_ARM32_PROTECT:value->memory->protect(packet.address,packet.length,packet.permission);break;
  case KLJ_ART_ARM32_TRANSLATE:packet.result=reinterpret_cast<uintptr_t>(value->memory->translate(packet.address,packet.length,packet.permission));break;
  case KLJ_ART_ARM32_LOAD_ADDRESS:packet.result=reinterpret_cast<uintptr_t>(value->memory->loadAddress(packet.address,packet.length));break;
  case KLJ_ART_ARM32_BORROW:packet.result=value->memory->borrow(reinterpret_cast<void*>(packet.buffer),packet.length,packet.permission,std::string_view(reinterpret_cast<const char*>(packet.function),packet.count));break;
  case KLJ_ART_ARM32_ADDRESS:packet.result=value->memory->addressOf(reinterpret_cast<void*>(packet.buffer));break;
  case KLJ_ART_ARM32_MAP_NATIVE:{
   const auto function=packet.function,cookie=packet.cookie;
   if(!callback||!function)throw std::runtime_error("Missing native memory lifetime continuation");
   auto storage=std::shared_ptr<void>(reinterpret_cast<void*>(packet.buffer),[callback,function,cookie](void*){
    // The original shared_ptr deleter has no error return. Its owner and
    // allocator execute on the original Bionic stack, never on the Mac ABI.
    KljArtArm32 release{};release.operation=KLJ_ART_ARM32_CALLBACK;release.cookie=cookie;
    callback(function,reinterpret_cast<uintptr_t>(&release));
   });
   packet.count=1; // The host now owns the one guest lifetime continuation.
   std::function<void(unsigned)> prepare;
   if(packet.reserved&4)prepare=[callback,function,cookie](unsigned permission){continuation(callback,function,cookie,uint64_t(permission)+1);};
   packet.result=value->memory->mapNative(reinterpret_cast<void*>(packet.buffer),packet.length,packet.permission,
    std::move(storage),packet.address,packet.reserved&1,packet.reserved&2,std::move(prepare),packet.reserved&8);break;
  }
  case KLJ_ART_ARM32_RETIRE_NATIVE:value->memory->retireNative(reinterpret_cast<void*>(packet.buffer),packet.budget);break;
  case KLJ_ART_ARM32_INVALIDATE:value->memory->invalidate(packet.address,packet.length);break;
  case KLJ_ART_ARM32_MAPPING_DESCRIPTION:{
   const auto text=value->memory->mappingDescription(packet.address);packet.result=text.size();
   if(packet.buffer){if(packet.length<text.size()+1)throw std::runtime_error("ARM32 mapping description buffer is too small");std::memcpy(reinterpret_cast<void*>(packet.buffer),text.c_str(),text.size()+1);}break;
  }
  case KLJ_ART_ARM32_MAPPINGS:{
   const auto mappings=value->memory->mappings();packet.result=mappings.size();
   if(packet.buffer&&!mappings.empty()&&packet.count>=mappings.size())
    std::memcpy(reinterpret_cast<void *>(packet.buffer),mappings.data(),mappings.size()*sizeof(Memory::Mapping));
   break;
  }
  case KLJ_ART_ARM32_STRING:{
   const auto string=value->memory->string(packet.address);packet.result=string.size();
   if(packet.buffer){if(packet.length<string.size()+1)throw std::runtime_error("ARM32 string buffer is too small");std::memcpy(reinterpret_cast<void*>(packet.buffer),string.c_str(),string.size()+1);}break;
  }
  case KLJ_ART_ARM32_GATE:{
   const auto function=packet.function,cookie=packet.cookie;
   packet.result=value->memory->import([callback,function,cookie](Thread &){return continuation(callback,function,cookie);});break;
  }
  case KLJ_ART_ARM32_RETIRE_GATE:value->memory->retireImport(packet.address);break;
  case KLJ_ART_ARM32_ARGUMENT:packet.result=value->thread->argument(packet.address);break;
  case KLJ_ART_ARM32_ARGUMENTS:
   if(packet.count&&!packet.buffer)throw std::runtime_error("Missing ARM32 argument destination");
   value->thread->arguments(packet.address,std::span(reinterpret_cast<uint32_t*>(packet.buffer),packet.count));break;
  case KLJ_ART_ARM32_RETURN_ADDRESS:packet.result=value->thread->returnAddress();break;
  case KLJ_ART_ARM32_SYSCALL_ARGUMENT:packet.result=value->thread->syscallArgument(packet.address);break;
  case KLJ_ART_ARM32_TLS:packet.result=value->thread->tls();break;
  case KLJ_ART_ARM32_SET_TLS:value->thread->setTls(packet.address);break;
  case KLJ_ART_ARM32_SET_SYSCALL:{
   if(!packet.function)value->thread->setSyscall({});
   else{const auto function=packet.function,cookie=packet.cookie;
    value->thread->setSyscall([callback,function,cookie](Thread &,uint32_t number){return int64_t(continuation(callback,function,cookie,number));});}break;
  }
  case KLJ_ART_ARM32_IMAGE:{
   const auto function=packet.function,cookie=packet.cookie;
   auto image=std::make_shared<Owner>();image->memory=value->memory;
   auto bytes=std::span(reinterpret_cast<const uint8_t*>(packet.buffer),packet.length);
   if(packet.reserved)image->image=std::make_unique<Image>(*image->memory,bytes,packet.address);
   else image->image=std::make_unique<Image>(*image->memory,bytes,packet.address,
    [callback,function,cookie](std::string_view name){std::string terminated(name);return uint32_t(continuation(callback,function,cookie,reinterpret_cast<uintptr_t>(terminated.c_str())));});
   packet.result=publish(std::move(image));break;
  }
  case KLJ_ART_ARM32_IMAGE_RELOCATE:{
   const auto function=packet.function,cookie=packet.cookie;
   value->image->relocate([callback,function,cookie](std::string_view name){std::string terminated(name);return uint32_t(continuation(callback,function,cookie,reinterpret_cast<uintptr_t>(terminated.c_str())));});break;
  }
  case KLJ_ART_ARM32_IMAGE_INITIALIZE:value->image->initialize(*owner(packet.parent)->thread);break;
  case KLJ_ART_ARM32_IMAGE_SYMBOL:packet.result=value->image->symbol(reinterpret_cast<const char*>(packet.buffer));break;
  case KLJ_ART_ARM32_IMAGE_RUNPATH:{const auto text=value->image->runpath();packet.result=text.size();if(packet.buffer){if(packet.length<text.size()+1)throw std::runtime_error("ARM32 runpath buffer is too small");std::memcpy(reinterpret_cast<void *>(packet.buffer),text.c_str(),text.size()+1);}break;}
  case KLJ_ART_ARM32_IMAGE_BASE:packet.result=value->image->base();break;
  case KLJ_ART_ARM32_IMAGE_SPAN:packet.result=value->image->span();break;
  case KLJ_ART_ARM32_IMAGE_HEADERS:{unsigned count=0;packet.result=reinterpret_cast<uintptr_t>(value->image->programHeaders(&count));packet.count=count;break;}
  case KLJ_ART_ARM32_IMPORT:{
   if(!callback||!packet.function)throw std::runtime_error("Missing ART guest continuation");
   const auto function=packet.function,cookie=packet.cookie;const auto count=packet.count;
   packet.result=value->memory->import([callback,function,cookie,count](Thread &thread){
    std::vector<uint32_t> words(count);
    for(uint32_t i=0;i<count;i++)words[i]=thread.argument(i);
    KljArtArm32Callback request{cookie,reinterpret_cast<uintptr_t>(words.data()),count,0};
    return callback(function,reinterpret_cast<uintptr_t>(&request));
   });break;
  }
  case KLJ_ART_ARM32_CALL:{
   if(!value->thread||value->creator!=std::this_thread::get_id())throw std::runtime_error("ARM32 call requires its original thread owner");
   if(packet.count&&!packet.buffer)throw std::runtime_error("Missing ARM32 call arguments");
   const auto *input=reinterpret_cast<const KljArtArm32Value*>(packet.buffer);
   std::vector<Value> arguments;arguments.reserve(packet.count);
   for(uint32_t i=0;i<packet.count;i++)arguments.push_back({input[i].bits,input[i].words,input[i].alignment});
   packet.result=value->thread->call(packet.address,arguments,packet.budget?packet.budget:UINT64_MAX);break;
  }
  default:return -38;
  }
  return 0;
 }catch(const std::bad_alloc &){return -12;}
 catch(const std::exception &error){std::strncpy(packet.error,error.what(),sizeof(packet.error)-1);packet.error[sizeof(packet.error)-1]=0;return -22;}
}
