#include "runtime.h"
#include "arm32.h"
#include "jni-bridge.h"
#include "native-imports.h"
#include "managed-exit.h"
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <cstdio>
#include <dlfcn.h>

namespace maclauncher::arm32 {
namespace {
struct Process;
const KljJniInvocationApi *sharedInvocation() {
  auto owner=reinterpret_cast<const KljJniInvocationApi *(*)()>(dlsym(RTLD_DEFAULT,"klj_jni_invocation_api"));
  return owner?owner():nullptr;
}
// The shared kl_image owns this representation. There is no second image
// registry, dependency policy, constructor state or rejected-library state.
struct Representation { Process &process; std::unique_ptr<Image> image; std::string runpath; };
std::vector<uint8_t> readFile(const char *filename) {
  std::ifstream stream(filename,std::ios::binary);
  if(!stream)throw std::runtime_error("Android ARM32 library could not be opened");
  return {std::istreambuf_iterator<char>(stream),{}};
}
void failure(JNIEnv *env,const std::exception &error) {
  if(env->ExceptionCheck())return;
  auto type=env->FindClass("java/lang/UnsatisfiedLinkError");
  if(type)env->ThrowNew(type,error.what());
}
struct Process {
  Memory memory;
  Imports imports;
  Jni jni;
  JavaVM *vm;
  void *(*guestSymbol)(const char *);
  std::mutex addressMutex;
  uint32_t nextBias=0x10000000;
  KlImageFormat format{};
  Process(JavaVM *vm,void *(*lookup)(const char *),void *(*guestSymbol)(const char *),Jni::Metadata metadata={})
    :imports(memory,lookup),jni(memory,vm,metadata,reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT,"klj_enter_guest_thread")),sharedInvocation()),vm(vm),guestSymbol(guestSymbol) {
    if(auto api=sharedInvocation())imports.setThreadExitProbe(api->exitRequested);
    jni.setSyscall([this](Thread &thread,uint32_t number){return imports.syscall(thread,number);});
    imports.setThreadFactory([this]{return jni.thread();});
    imports.setCallback([this](Thread &thread,uint32_t function,std::span<const Value> arguments){return jni.callback(thread,function,arguments);});
    imports.setJniAdapter([this](JniValue kind,uint64_t value,bool toHost){return jni.nativeValue(kind,value,toHost);});
    auto observe=reinterpret_cast<void (*)(void (*)(void *,void *),void *)>(dlsym(RTLD_DEFAULT,"klj_pthread_observe_retirement"));
    if(observe)observe([](void *context,void *thread){static_cast<Process *>(context)->imports.retireThread(thread);},this);
    format={1,this,map,unmap,symbol,construct,needed,phdrs,dependencies,mapUnlinked,relocate,runpath};
  }
  static void *map(void *context,const char *filename,uintptr_t *base,size_t *span,char *error,size_t capacity) {
    void *value=mapUnlinked(context,filename,base,span,error,capacity);
    if(value&&!relocate(value,nullptr,nullptr,error,capacity)){unmap(value);return nullptr;}
    return value;
  }
  static void *mapUnlinked(void *context,const char *filename,uintptr_t *base,size_t *span,char *error,size_t capacity) {
    try {
      auto &self=*static_cast<Process *>(context);auto bytes=readFile(filename);
      auto layout=Image::inspect(bytes);uint32_t bias;
      {
        std::lock_guard lock(self.addressMutex);bias=self.nextBias;
        uint64_t next=(uint64_t(bias)+layout.span+0xffff)&~UINT64_C(0xffff);
        if(next>0x50000000)throw std::runtime_error("Android ARM32 library address space is full");
        self.nextBias=uint32_t(next);
      }
      auto value=std::make_unique<Representation>(Representation{self,{},{}});
      value->image=std::make_unique<Image>(self.memory,bytes,bias);
      value->runpath=value->image->runpath();
      *base=value->image->base();*span=value->image->span();return value.release();
    } catch(const std::exception &e){std::snprintf(error,capacity,"%s",e.what());return nullptr;}
  }
  static int relocate(void *value,void *(*lookup)(void *,const char *),void *context,char *error,size_t capacity){
    try{
      auto &r=*static_cast<Representation *>(value);auto &self=r.process;
      r.image->relocate([&self,lookup,context](std::string_view name){
        void *address=lookup?lookup(context,std::string(name).c_str()):nullptr;
        if(!address&&self.guestSymbol)address=self.guestSymbol(std::string(name).c_str());
        if(address) {
          auto value=reinterpret_cast<uintptr_t>(address);
          if(value<=UINT32_MAX)return uint32_t(value);
          return self.imports.resolve(name,address);
        }
        return self.imports.resolve(name);
      });
      return 1;
    }catch(const std::exception &e){std::snprintf(error,capacity,"%s",e.what());return 0;}
  }
  static void unmap(void *value){delete static_cast<Representation *>(value);}
  static const char *runpath(void *value){return static_cast<Representation *>(value)->runpath.c_str();}
  static void *symbol(void *value,const char *name) {
    return reinterpret_cast<void *>(uintptr_t(static_cast<Representation *>(value)->image->symbol(name)));
  }
  static int construct(void *value) {
    auto &r=*static_cast<Representation *>(value);JNIEnv *env=nullptr;
    if(r.process.vm->GetEnv(reinterpret_cast<void **>(&env),JNI_VERSION_1_6)!=JNI_OK)return 0;
    try {auto thread=r.process.jni.thread();r.process.jni.construct(*thread,env,*r.image);return !env->ExceptionCheck();}
    catch(const ManagedThreadExit &){return 0;}
    catch(const std::exception &e){failure(env,e);return 0;}
  }
  static int needed(void *,const char *filename,char (*names)[128],int maximum,char *error,size_t capacity) {
    try {
      auto layout=Image::inspect(readFile(filename));int count=0;
      for(auto &name:layout.needed){if(count==maximum)break;std::snprintf(names[count++],128,"%s",name.c_str());}
      return count;
    }catch(const std::exception &e){std::snprintf(error,capacity,"%s",e.what());return -1;}
  }
  static const void *phdrs(void *value,unsigned *count){return static_cast<Representation *>(value)->image->programHeaders(count);}
  static int dependencies(void *,const char *filename,int (*visit)(void *,const char *),void *context,char *error,size_t capacity){
    try{for(auto &name:Image::inspect(readFile(filename)).needed)if(!visit(context,name.c_str()))return 0;return 1;}
    catch(const std::exception &e){std::snprintf(error,capacity,"%s",e.what());return 0;}
  }
};
void *create(JNIEnv *env,void *(*lookup)(const char *),void *(*guestSymbol)(const char *)) {
  try {JavaVM *vm=nullptr;if(env->GetJavaVM(&vm)!=JNI_OK)return nullptr;return new Process(vm,lookup,guestSymbol);}
  catch(const std::exception &e){failure(env,e);return nullptr;}
}
const KlImageFormat *format(void *process){return &static_cast<Process *>(process)->format;}
int bind(JNIEnv *env,void *opaque,jclass target,const char *const *names,
         const char *const *descriptors,const char *const *shortNames,
         const char *const *longNames,size_t count) {
  try {
    auto &r=*static_cast<Representation *>(opaque);std::vector<Jni::NativeMethod> methods;
    for(size_t i=0;i<count;i++) {
      auto function=r.image->symbol(shortNames[i]);if(!function)function=r.image->symbol(longNames[i]);
      if(!function)throw std::runtime_error("Missing JNI export: "+std::string(shortNames[i])+" or "+longNames[i]);
      methods.push_back({names[i],descriptors[i],function});
    }
    return r.process.jni.bind(env,target,methods)==JNI_OK?int(count):0;
  }catch(const std::exception &e){failure(env,e);return 0;}
}
void initialize(JNIEnv *env,void *opaque) {
  try {
    auto &r=*static_cast<Representation *>(opaque);auto function=r.image->symbol("JNI_OnLoad");if(!function)return;
    auto thread=r.process.jni.thread();auto version=r.process.jni.initialize(*thread,env,function);
    if(version<0)throw std::runtime_error("Guest JNI_OnLoad failed");
  }catch(const ManagedThreadExit &){}
   catch(const std::exception &e){failure(env,e);}
}
void *createMetadata(JNIEnv *env,void *(*lookup)(const char *),void *(*guestSymbol)(const char *),const char *(*shorty)(JNIEnv *,jmethodID)){
  try{JavaVM *vm=nullptr;if(!shorty||env->GetJavaVM(&vm)!=JNI_OK)return nullptr;return new Process(vm,lookup,guestSymbol,Jni::Metadata{shorty,true});}
  catch(const std::exception &e){failure(env,e);return nullptr;}
}
void *trampoline(void *process,uint32_t function,const char *shorty,size_t length,unsigned type){
  if(type>unsigned(Jni::NativeCall::Invocation))throw std::runtime_error("Unknown JNI native call convention");
  return static_cast<Process *>(process)->jni.trampoline(function,std::string_view(shorty,length),Jni::NativeCall(type));
}
int executable(void *process,uintptr_t address){
  if(!address||address>UINT32_MAX)return 0;
  try{return static_cast<Process *>(process)->memory.translate(uint32_t(address)&~1u,2,Memory::Execute)!=nullptr;}
  catch(const Memory::Fault &){return 0;}
}
uintptr_t caller(void *process){return static_cast<Process *>(process)->imports.caller();}
const klj_arm32_runtime_api api={4,create,format,bind,initialize,createMetadata,trampoline,executable,caller};
}
}
extern "C" const klj_arm32_runtime_api *klj_arm32_runtime(void){return &maclauncher::arm32::api;}
