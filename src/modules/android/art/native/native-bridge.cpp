#include <nativebridge/native_bridge.h>
#include <nativeloader/dlext_namespaces.h>
#include "../../native/arm32/runtime.h"
#include "../../native/arm32/jni-bridge.h"
#include "../../native/library-registry.h"
#include "../../native/library-namespace.h"
#include "../../native/image-constructor.h"
#include "../../native/proc-maps.h"
#include "arm32-components.h"
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

// ART supplies class-loader namespaces, JNI metadata and JNI_OnLoad ordering.
// The same image registry, format/CPU/JNI adapters and constructor state used
// by the JVM own translated code. Original Bionic owns every native ELF64
// dependency and its namespace; no platform libc is loaded by this adapter.
namespace {
using android::native_bridge_namespace_t;
using android::JNICallType;
using maclauncher::arm32::Jni;
#define LOG_ALWAYS_FATAL_IF(condition,...) do{if(condition)throw std::runtime_error("Original runtime has an invalid APEX location");}while(false)
#include "art-namespace.inc"
#undef LOG_ALWAYS_FATAL_IF
thread_local std::string lastError;
thread_local std::string returnedError;
const android::NativeBridgeRuntimeCallbacks *runtimeCallbacks;
void *dynamicSymbol(const char *);
void *nativeSymbol(const char *);
bool supported(const char *path){
 if(!path)return false;FILE *file=std::fopen(path,"rb");if(!file)return false;
 unsigned char bytes[20];size_t size=std::fread(bytes,1,sizeof bytes,file);std::fclose(file);
 return size==sizeof bytes&&!std::memcmp(bytes,"\177ELF",4)&&bytes[4]==1&&bytes[5]==1&&bytes[18]==40&&bytes[19]==0;
}
bool supportedPath(const char *paths){
 if(!paths)return false;std::string list(paths);size_t start=0;
 while(start<list.size()){
  size_t end=list.find(':',start);if(end==std::string::npos)end=list.size();auto path=list.substr(start,end-start);
  if(DIR *directory=opendir(path.c_str())){
   bool found=false;while(auto *entry=readdir(directory))if(supported((path+"/"+entry->d_name).c_str())){found=true;break;}
   closedir(directory);if(found)return true;
  }start=end+1;
 }return false;
}
KljLibraryNamespace *space(native_bridge_namespace_t *value){return reinterpret_cast<KljLibraryNamespace *>(value);}
native_bridge_namespace_t *handle(KljLibraryNamespace *value){return reinterpret_cast<native_bridge_namespace_t *>(value);}
android_namespace_t *native(KljLibraryNamespace *value){return static_cast<android_namespace_t *>(klj_namespace_native(value));}
void *platformOpen(KljLibraryNamespace *space,const char *path,int flags){
 android_dlextinfo info{};info.flags=ANDROID_DLEXT_USE_NAMESPACE;info.library_namespace=native(space);
 return info.library_namespace?android_dlopen_ext(path,flags,&info):dlopen(path,flags);
}
struct Image {
 std::string path,runpath;KljLibraryNamespace *space;const KlImageFormat *format;void *payload=nullptr;
 KlImageInitialization initialization{};
 std::vector<Image *> children;
 std::vector<void *> nativeDependencies;
 unsigned references=0;
 uintptr_t base=0;size_t span=0;
 Image *group=nullptr;
 bool global=false;
 ~Image(){if(payload)format->unmap(payload);for(auto *library:nativeDependencies)dlclose(library);}
 int initialize(){return klj_image_construct(&initialization,[](void *opaque){
  auto &image=*static_cast<Image *>(opaque);
  for(auto *child:image.children)if(!child->initialize())return 0;
  return image.format->initialize(image.payload);
 },this);}
};
struct Loader {
 const klj_arm32_runtime_api *api=klj_arm32_runtime();
 void *process=nullptr;const KlImageFormat *format=nullptr;
 KlImageRegistry *registry=klj_library_registry_create();
 KljLibraryNamespaces *namespaces=klj_namespaces_create();
 KljLibraryNamespace *defaultSpace=nullptr;
 pthread_mutex_t mapping=PTHREAD_MUTEX_INITIALIZER;
 std::vector<Image *> preloads;
 Loader(){
  if(!registry||!namespaces)throw std::bad_alloc();
  JavaVM *vm=nullptr;jsize count=0;
  jint (*get)(JavaVM **,jsize,jsize *)=nullptr;
  void *runtime=nullptr;
  std::string runtimeError;
  {
   // libart may be RTLD_LOCAL. Resolve its invocation API from the actual
   // runtime callback's image, never load a different VM or guess its path.
   Dl_info location{};
   if(dladdr(reinterpret_cast<void *>(runtimeCallbacks->getMethodShorty),&location)&&location.dli_fname){
    runtime=dlopen(location.dli_fname,RTLD_NOW|RTLD_NOLOAD);
    if(!runtime){
     // libnativebridge loads this adapter in the system namespace. Resolve
     // the callback's existing APEX through original NativeLoader policy.
     const auto name=FindApexNamespaceName(location.dli_fname);
     android_dlextinfo info{};info.flags=ANDROID_DLEXT_USE_NAMESPACE;
     info.library_namespace=android_get_exported_namespace(name?name->c_str():"default");
     if(info.library_namespace)runtime=android_dlopen_ext(location.dli_fname,RTLD_NOW|RTLD_NOLOAD,&info);
    }
    if(!runtime){const char *failure=dlerror();runtimeError=failure?failure:"Runtime namespace has no retained library handle";}
   }else runtimeError="Runtime metadata callback has no original ELF identity";
   if(runtime)get=reinterpret_cast<decltype(get)>(dlsym(runtime,"JNI_GetCreatedJavaVMs"));
  }
  if(!get)get=reinterpret_cast<decltype(get)>(dlsym(RTLD_DEFAULT,"JNI_GetCreatedJavaVMs"));
  JNIEnv *env=nullptr;
  jint invocation=get?get(&vm,1,&count):JNI_ERR;
  jint environment=invocation==JNI_OK&&count==1?vm->GetEnv(reinterpret_cast<void **>(&env),JNI_VERSION_1_6):JNI_ERR;
  if(runtime)dlclose(runtime);
  if(environment!=JNI_OK)
   throw std::runtime_error("Original ART JNI environment is unavailable (invocation="+std::to_string(invocation)+", VMs="+std::to_string(count)+", environment="+std::to_string(environment)+"): "+runtimeError);
  process=api->create_metadata(env,nativeSymbol,nullptr,runtimeCallbacks->getMethodShorty);
  if(!process)throw std::runtime_error("Shared native execution owner could not initialize");
  format=api->format(process);
  defaultSpace=klj_namespace_create(namespaces,"default",getenv("LD_LIBRARY_PATH"),nullptr,0,nullptr,nullptr,android_get_exported_namespace("default"));
  if(!defaultSpace)throw std::runtime_error(klj_namespace_error());klj_namespace_export(defaultSpace);
  // Both VM bindings load the same original guest-width compiler/ABI image.
  // Its global membership is inherited by original class-loader namespaces;
  // pointer-bearing legacy data never becomes a host-width approximation.
  for(const char *name:klj_arm32_preloads){
  const std::string builtins=std::string("/system/lib/")+name;
  
  auto ops=operations();char error[1024];
  auto *image=static_cast<Image *>(klj_library_group_load(&mapping,registry,&ops,defaultSpace,builtins.c_str(),name,nullptr,nullptr,error,sizeof error));
  if(!image)throw std::runtime_error(error);
  
  if(!klj_namespace_publish(defaultSpace,image,1))throw std::runtime_error(klj_namespace_error());
  image->global=true;++image->references;
  preloads.push_back(image);
  }
 }
 Image *image(void *handle){auto values=klj_library_registry_snapshot(registry);for(size_t i=0;i<values.count;i++)if(values.entries[i].img==handle)return static_cast<Image *>(handle);return nullptr;}
 Image *caller(){auto pc=api->caller(process)&~uintptr_t(1);auto values=klj_library_registry_snapshot(registry);for(size_t i=0;i<values.count;i++){auto *image=static_cast<Image *>(values.entries[i].img);if(pc>=image->base&&pc-image->base<image->span)return image;}return nullptr;}
 static int candidate(void *,const char *path,char *result,size_t capacity){
  if(!supported(path))return 0;std::unique_ptr<char,decltype(&free)> canonical(realpath(path,nullptr),free);
  if(!canonical||std::strlen(canonical.get())>=capacity)return 0;std::strcpy(result,canonical.get());return 1;
 }
 KljLibraryGroupOperations operations(){return {this,
  [](void *,void *image){return static_cast<Image *>(image)->path.c_str();},
  [](void *,void *ns,void *image){return klj_namespace_contains(static_cast<KljLibraryNamespace *>(ns),image);},
  [](void *opaque,void *ns,const char *path,char *error,size_t capacity)->void *{
   auto &self=*static_cast<Loader *>(opaque);auto image=std::make_unique<Image>();image->path=path;image->space=static_cast<KljLibraryNamespace *>(ns);image->format=self.format;
   image->payload=self.format->map_unlinked(self.process,path,&image->base,&image->span,error,capacity);
   if(image->payload)image->runpath=self.format->runpath(image->payload);
   return image->payload?image.release():nullptr;
  },
  [](void *,void *image){delete static_cast<Image *>(image);},
  [](void *opaque,const char *path,int (*visit)(void *,const char *),void *context,char *error,size_t capacity){auto &self=*static_cast<Loader *>(opaque);return self.format->dependencies(self.process,path,visit,context,error,capacity);},
  [](void *,void *ns,void *requester_image,const char *requester,const char *name,void **provider,char *path,size_t capacity,char *error,size_t errorCapacity){
   auto &image=*static_cast<Image *>(requester_image);auto *owner=static_cast<KljLibraryNamespace *>(ns);
   if(auto *found=klj_namespace_resolve_from(owner,name,requester,image.runpath.c_str(),image.format->elf_class,candidate,nullptr,path,capacity)){*provider=found;return 1;}
   void *library=platformOpen(owner,name,RTLD_NOW|RTLD_LOCAL);
   if(!library){const char *failure=dlerror();std::snprintf(error,errorCapacity,"%s",failure?failure:"Native dependency could not load");return 0;}
   try{image.nativeDependencies.push_back(library);}catch(...){dlclose(library);throw;}return 2;
  },
  [](void *,void *image,const char *name)->void *{auto &value=*static_cast<Image *>(image);return value.format->symbol(value.payload,name);},
  [](void *,void *ns,void *image){return klj_namespace_global(static_cast<KljLibraryNamespace *>(ns),image);},
  [](void *,void *image,void *(*lookup)(void *,const char *),void *context,char *error,size_t capacity){auto &value=*static_cast<Image *>(image);return value.format->relocate(value.payload,lookup,context,error,capacity);},
  [](void *,void *image,void *const *children,size_t count){auto &value=*static_cast<Image *>(image);for(size_t i=0;i<count;i++)value.children.push_back(static_cast<Image *>(children[i]));return 1;},
  nullptr,
  [](void *opaque,const KlImageRegistryEntry *entries,void *const *spaces,size_t count){
   auto &self=*static_cast<Loader *>(opaque);
   for(size_t i=0;i<count;i++)if(!klj_namespace_publish(static_cast<KljLibraryNamespace *>(spaces[i]),entries[i].img,0))std::abort();
   if(!klj_library_registry_publish_group(self.registry,entries,count))std::abort();return 1;
  },
  [](void *,void *image,const char *name)->void *{if(void *value=dynamicSymbol(name))return value;for(void *library:static_cast<Image *>(image)->nativeDependencies)if(void *value=dlsym(library,name))return value;return nullptr;},
  [](void *,void *image,void (*visit)(void *,void *),void *context){for(auto *child:static_cast<Image *>(image)->children)visit(context,child);},
  [](void *,void *image,void *root){static_cast<Image *>(image)->group=static_cast<Image *>(root);}
 };}
};
// Constructor-time libc queries precede publication. Subsequent native ABI
// metadata queries use the caller's same retained class-loader group, whose
// RTLD_LOCAL dependencies must not become globally visible.
std::atomic<Loader *> publishedLoader{nullptr};
int mapsImage(uint64_t address,KljProcMapFile *result){
 auto *self=publishedLoader.load(std::memory_order_acquire);if(!self)return 0;
 const auto images=klj_library_registry_snapshot(self->registry);
 uint64_t best=UINT64_MAX;int found=0;
 for(size_t index=0;index<images.count;index++){
  const auto &image=*static_cast<Image *>(images.entries[index].img);
  unsigned count=0;const auto *headers=static_cast<const KlProgramHeader *>(image.format->phdrs(image.payload,&count));
  KljProcMapFile candidate;
  if(!klj_proc_maps_elf_file(image.base,headers,count,4096,image.path.c_str(),image.path.c_str(),address,&candidate))continue;
  const auto at=std::max(candidate.start,address);
  if(at<best){*result=candidate;best=at;found=1;}
 }
 return found;
}
Loader &loader(){static Loader *value=[] {auto *result=new Loader;publishedLoader.store(result,std::memory_order_release);return result;}();return *value;}
void *nativeSymbol(const char *name){
 if(void *value=dynamicSymbol(name))return value;
 if(auto *self=publishedLoader.load(std::memory_order_acquire))if(auto *caller=self->caller()){
  auto ops=self->operations();
  // Imports calls native functions/data directly. Translated guest exports
  // remain with the ordinary relocation/dlsym path, never this native lookup.
  ops.symbol=[](void *,void *,const char *)->void *{return nullptr;};
  auto *root=caller->group?caller->group:caller;
  if(void *value=klj_library_group_symbol(&ops,root->space,root,nullptr,name))return value;
 }
 return dlsym(RTLD_DEFAULT,name);
}
template<class T,class F>T guarded(T failure,F body){try{return body();}catch(const std::exception &error){lastError=error.what();std::fprintf(stderr,"[android-native-bridge] %s\n",error.what());return failure;}}
bool initialize(const android::NativeBridgeRuntimeCallbacks *callbacks,const char *,const char *){
 if(!callbacks||!callbacks->getMethodShorty)return false;runtimeCallbacks=callbacks;return true;
}
void *load(const char *path,int flags,native_bridge_namespace_t *ns){return guarded<void *>(nullptr,[&]()->void *{auto &self=loader();auto *owner=ns?space(ns):self.defaultSpace;char canonical[4096];
 
 auto *provider=klj_namespace_resolve(owner,path,nullptr,Loader::candidate,nullptr,canonical,sizeof canonical);
 if(!provider){lastError="Native library is unavailable in its class-loader namespace";return nullptr;}
 if(flags&RTLD_NOLOAD){auto values=klj_library_registry_snapshot(self.registry);for(size_t i=0;i<values.count;i++){auto *image=static_cast<Image *>(values.entries[i].img);if(image->path==canonical&&klj_namespace_contains(provider,image)){
 pthread_mutex_lock(&self.mapping);++image->references;pthread_mutex_unlock(&self.mapping);return image;}}lastError="Native library is not loaded";return nullptr;}
 
 // NativeBridge namespace discovery precedes AndroidRuntime JNI registration.
 // Match ordinary ELF loading: initialize the shared preloads when actual
 // guest code is first loaded, while namespace membership is already known.
 for(auto *image:self.preloads)if(!image->initialize()){lastError="Original ARM32 compiler runtime initialization failed";return nullptr;}
 auto ops=self.operations();char error[1024];
 auto *image=static_cast<Image *>(klj_library_group_load(&self.mapping,self.registry,&ops,provider,canonical,path,nullptr,nullptr,error,sizeof error));
 if(!image){lastError=error;return nullptr;}
 
 if(!image->initialize()){lastError="Native library constructor failed";return nullptr;}
 pthread_mutex_lock(&self.mapping);++image->references;
 if(flags&RTLD_GLOBAL){image->global=true;if(!klj_namespace_publish(provider,image,1))std::abort();}
 pthread_mutex_unlock(&self.mapping);return image;
 });}
int closeImage(void *opaque){auto &self=loader();auto *image=self.image(opaque);if(!image)return dlclose(opaque);pthread_mutex_lock(&self.mapping);int result=image->references?--image->references,0:-1;pthread_mutex_unlock(&self.mapping);return result;}
void *dynamicOpen(const char *path,int flags){return guarded<void *>(nullptr,[&]()->void *{
 auto &self=loader();auto *caller=self.caller();auto *owner=caller?caller->space:self.defaultSpace;
 if(path){char candidate[4096];if(auto *provider=klj_namespace_resolve_from(owner,path,caller?caller->path.c_str():nullptr,caller?caller->runpath.c_str():nullptr,1,Loader::candidate,nullptr,candidate,sizeof candidate))return load(candidate,flags,handle(provider));}
 return platformOpen(owner,path,flags);
 });}
void *dynamicLookup(void *opaque,const char *name){return guarded<void *>(nullptr,[&]()->void *{
 auto &self=loader();auto *image=self.image(opaque);auto ops=self.operations();
 if(image){void *value=klj_library_group_symbol(&ops,image->space,image,nullptr,name);if(!value)lastError=std::string("Native symbol unavailable: ")+name;return value;}
 if(opaque!=RTLD_DEFAULT&&opaque!=RTLD_NEXT)return dlsym(opaque,name);
 auto *caller=self.caller();auto *owner=caller?caller->space:self.defaultSpace;bool skip=opaque==RTLD_NEXT;
 if(skip&&!caller)return nullptr;
 auto values=klj_library_registry_snapshot(self.registry);
 for(size_t i=0;i<values.count;i++){auto *entry=static_cast<Image *>(values.entries[i].img);if(skip){skip=entry!=caller;continue;}if(klj_namespace_global(owner,entry)){if(void *value=entry->format->symbol(entry->payload,name)){return value;}}}
 if(caller)if(void *value=klj_library_group_symbol(&ops,caller->group->space,caller->group,opaque==RTLD_NEXT?caller:nullptr,name))return value;
 return dlsym(RTLD_DEFAULT,name);
 });}
const char *dynamicError(){if(lastError.empty())return dlerror();returnedError=std::move(lastError);lastError.clear();return returnedError.c_str();}
void *dynamicSymbol(const char *name){
 if(!std::strcmp(name,"klj_proc_maps_image"))return reinterpret_cast<void *>(mapsImage);
 if(!std::strcmp(name,"dlopen"))return reinterpret_cast<void *>(dynamicOpen);
 if(!std::strcmp(name,"dlsym"))return reinterpret_cast<void *>(dynamicLookup);
 if(!std::strcmp(name,"dlclose"))return reinterpret_cast<void *>(+[](void *image){return guarded(-1,[&]{return closeImage(image);});});
 if(!std::strcmp(name,"dlerror"))return reinterpret_cast<void *>(dynamicError);
 return nullptr;
}
void *trampoline(void *opaque,const char *name,const char *shorty,uint32_t length,JNICallType type){return guarded<void *>(nullptr,[&]()->void *{
 auto &self=loader();auto &image=*static_cast<Image *>(opaque);auto ops=self.operations();
 auto function=reinterpret_cast<uintptr_t>(klj_library_group_symbol(&ops,image.space,&image,nullptr,name));
 if(!function){lastError=std::string("Native symbol unavailable: ")+name;return nullptr;}
 if(!self.api->executable(self.process,function))return reinterpret_cast<void *>(function);
 unsigned convention=type==android::kJNICallTypeCriticalNative?unsigned(Jni::NativeCall::Critical):unsigned(Jni::NativeCall::Regular);
 if(shorty&&!length)length=uint32_t(std::strlen(shorty));
 if(!shorty&&(!std::strcmp(name,"JNI_OnLoad")||!std::strcmp(name,"JNI_OnUnload"))){shorty=!std::strcmp(name,"JNI_OnLoad")?"I":"V";length=1;convention=unsigned(Jni::NativeCall::Invocation);}
 return self.api->trampoline(self.process,uint32_t(function),shorty,length,convention);
 });}
native_bridge_namespace_t *createNamespace(const char *name,const char *ld,const char *defaults,uint64_t type,const char *permitted,native_bridge_namespace_t *parent){
 return guarded<native_bridge_namespace_t *>(nullptr,[&]{auto &self=loader();auto *original=android_create_namespace(name,ld,defaults,type,permitted,parent?native(space(parent)):native(self.defaultSpace));if(!original){const char *error=dlerror();throw std::runtime_error(error?error:"Original native namespace creation failed");}
  auto *value=klj_namespace_create(self.namespaces,name,ld,defaults,type,permitted,parent?space(parent):self.defaultSpace,original);if(!value)lastError=klj_namespace_error();return handle(value);});
}
bool linkNamespaces(native_bridge_namespace_t *from,native_bridge_namespace_t *to,const char *names){return guarded(false,[&]{
 auto &self=loader();auto *first=from?space(from):self.defaultSpace,*second=to?space(to):self.defaultSpace;
 if(!android_link_namespaces(native(first),native(second),names)){const char *error=dlerror();lastError=error?error:"Native namespace link failed";return false;}
 return klj_namespace_link(first,second,names,0)!=0;
 });}
native_bridge_namespace_t *exported(const char *name){return guarded<native_bridge_namespace_t *>(nullptr,[&]{auto &self=loader();if(auto *found=klj_namespace_exported(self.namespaces,name))return handle(found);
 auto *original=android_get_exported_namespace(name);if(!original)return static_cast<native_bridge_namespace_t *>(nullptr);
 auto *value=klj_namespace_create(self.namespaces,name,nullptr,nullptr,ANDROID_NAMESPACE_TYPE_ISOLATED,nullptr,nullptr,original);
 if(!value){lastError=klj_namespace_error();return static_cast<native_bridge_namespace_t *>(nullptr);}klj_namespace_export(value);return handle(value);
 });}
}
extern "C" __attribute__((visibility("default"))) android::NativeBridgeCallbacks NativeBridgeItf={
 .version=8,.initialize=initialize,
 .loadLibrary=+[](const char *path,int flags){return load(path,flags,nullptr);},
 .getTrampoline=+[](void *image,const char *name,const char *shorty,uint32_t size){return trampoline(image,name,shorty,size,android::kJNICallTypeRegular);},
 .isSupported=supported,
 // Only native library calls are translated; ART/libcore retain their actual
 // 64-bit environment. The shared package owner records the selected APK ABI.
 .getAppEnv=+[](const char *)->const android::NativeBridgeRuntimeValues *{return nullptr;},
 .isCompatibleWith=+[](uint32_t version){return version>=3&&version<=8;},
 .getSignalHandler=+[](int)->android::NativeBridgeSignalHandlerFn{return nullptr;},
 .unloadLibrary=+[](void *opaque){return guarded(-1,[&]{return closeImage(opaque);});},
 .getError=+[]{return lastError.empty()?nullptr:lastError.c_str();},
 .isPathSupported=supportedPath,.unused_initAnonymousNamespace=nullptr,
 .createNamespace=createNamespace,.linkNamespaces=linkNamespaces,.loadLibraryExt=load,
 .getVendorNamespace=+[]{return exported("vendor");},.getExportedNamespace=exported,.preZygoteFork=nullptr,
 .getTrampolineWithJNICallType=trampoline,
 .getTrampolineForFunctionPointer=+[](const void *function,const char *shorty,uint32_t length,JNICallType type)->void *{return guarded<void *>(nullptr,[&]{auto &self=loader();if(shorty&&!length)length=uint32_t(std::strlen(shorty));return self.api->trampoline(self.process,uint32_t(reinterpret_cast<uintptr_t>(function)),shorty,length,unsigned(type==android::kJNICallTypeCriticalNative?Jni::NativeCall::Critical:Jni::NativeCall::Regular));});},
 .isNativeBridgeFunctionPointer=+[](const void *function){return guarded(false,[&]{auto &self=loader();return self.api->executable(self.process,reinterpret_cast<uintptr_t>(function))!=0;});}
};
