#include "library-namespace.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
thread_local std::string lastError;
const char *baseName(const char *path){const char *slash=std::strrchr(path,'/');return slash?slash+1:path;}
std::vector<std::string> paths(const char *value){
 std::vector<std::string> result;if(!value)return result;
 const std::string_view text(value);size_t start=0;
 while(start<text.size()){
  auto end=text.find(':',start);if(end==std::string_view::npos)end=text.size();
  if(end>start)result.emplace_back(text.substr(start,end-start));start=end+1;
 }
 return result;
}
template<class T>void append(std::vector<T> &to,const std::vector<T>&from){to.insert(to.end(),from.begin(),from.end());}
}
struct KljLibraryNamespace {
 struct Link{KljLibraryNamespace *target;std::unordered_set<std::string> names;bool all;};
 struct Member{void *image;bool global;};
 KljLibraryNamespaces *owner;
 std::string name;
 bool is_isolated_=false;
 bool visible=false;
 std::vector<std::string> allowed_libs_,ld_library_paths_,default_library_paths_,permitted_paths_;
 std::vector<Link> links;
 std::vector<Member> members;
 void *native=nullptr;
 bool is_accessible(const std::string &file);
};
struct KljLibraryNamespaces {
 std::recursive_mutex mutex;
 std::vector<std::unique_ptr<KljLibraryNamespace>> values;
 KljLibraryNamespace *anonymous=nullptr;
};
// Exact pinned Bionic access rules and the original namespace flag declaration.
// The supplier adapts only the surrounding image/namespace representation.
#include "bionic-namespace.inc"

extern "C" KljLibraryNamespaces *klj_namespaces_create(void){
 try{return new KljLibraryNamespaces;}catch(const std::exception &error){lastError=error.what();return nullptr;}
}
extern "C" void klj_namespaces_destroy(KljLibraryNamespaces *owner){delete owner;}
extern "C" KljLibraryNamespace *klj_namespace_create(KljLibraryNamespaces *owner,const char *name,
 const char *library,const char *defaults,uint64_t flags,const char *permitted,KljLibraryNamespace *parent,void *native){
 try{
  if(!owner||!name)throw std::runtime_error("Native namespace owner or name is missing");
  std::lock_guard lock(owner->mutex);
  if(parent&&parent->owner!=owner)throw std::runtime_error("Native namespace has a different registry owner");
  if((flags&ANDROID_NAMESPACE_TYPE_ALSO_USED_AS_ANONYMOUS)&&owner->anonymous)
   throw std::runtime_error("An anonymous native namespace already exists");
  auto value=std::make_unique<KljLibraryNamespace>();value->owner=owner;value->name=name;value->native=native;
  value->is_isolated_=(flags&ANDROID_NAMESPACE_TYPE_ISOLATED)!=0;
  value->ld_library_paths_=paths(library);value->default_library_paths_=paths(defaults);value->permitted_paths_=paths(permitted);
  if(parent){
   if(flags&ANDROID_NAMESPACE_TYPE_SHARED){
    append(value->ld_library_paths_,parent->ld_library_paths_);append(value->default_library_paths_,parent->default_library_paths_);
    append(value->permitted_paths_,parent->permitted_paths_);value->links=parent->links;value->members=parent->members;
   }else for(auto member:parent->members)if(member.global)value->members.push_back(member);
  }
  auto *result=value.get();owner->values.push_back(std::move(value));
  if(flags&ANDROID_NAMESPACE_TYPE_ALSO_USED_AS_ANONYMOUS)owner->anonymous=result;
  return result;
 }catch(const std::exception &error){lastError=error.what();return nullptr;}
}
extern "C" int klj_namespace_paths(KljLibraryNamespace *value,const char *library,const char *defaults){
 try{if(!value)return 0;auto first=paths(library),second=paths(defaults);std::lock_guard lock(value->owner->mutex);
  value->ld_library_paths_=std::move(first);value->default_library_paths_=std::move(second);return 1;
 }catch(const std::exception &error){lastError=error.what();return 0;}
}
extern "C" int klj_namespace_link(KljLibraryNamespace *from,KljLibraryNamespace *to,const char *names,int all){
 try{
  if(!from||!to||from->owner!=to->owner)throw std::runtime_error("Native namespace link has a different registry owner");
  auto entries=paths(names);if(entries.empty()&&!all)throw std::runtime_error("Native namespace link requires library names");
  std::lock_guard lock(from->owner->mutex);from->links.push_back({to,{entries.begin(),entries.end()},all!=0});return 1;
 }catch(const std::exception &error){lastError=error.what();return 0;}
}
extern "C" KljLibraryNamespace *klj_namespace_exported(KljLibraryNamespaces *owner,const char *name){
 if(!owner||!name)return nullptr;std::lock_guard lock(owner->mutex);
 for(auto &value:owner->values)if(value->visible&&value->name==name)return value.get();return nullptr;
}
extern "C" void klj_namespace_export(KljLibraryNamespace *value){if(value){std::lock_guard lock(value->owner->mutex);value->visible=true;}}
extern "C" void *klj_namespace_native(KljLibraryNamespace *value){return value?value->native:nullptr;}
extern "C" int klj_namespace_accessible(KljLibraryNamespace *value,const char *file){
 if(!value||!file)return 0;std::lock_guard lock(value->owner->mutex);return value->is_accessible(file);
}
extern "C" KljLibraryNamespace *klj_namespace_resolve(KljLibraryNamespace *value,const char *name,
 const char *runpath,KljLibraryCandidate candidate,void *context,char *output,size_t capacity){
 try{
  if(!value||!name||!*name||!candidate||!output||!capacity)throw std::runtime_error("Native library search input is missing");
  std::lock_guard lock(value->owner->mutex);
  auto one=[&](KljLibraryNamespace *space,const std::vector<std::string> &run)->bool{
   auto accept=[&](const std::string &file){return candidate(context,file.c_str(),output,capacity)&&space->is_accessible(output);};
   if(std::strchr(name,'/'))return accept(name);
   const std::vector<std::string> *lists[]={&space->ld_library_paths_,&run,&space->default_library_paths_};
   for(const auto *list:lists)
    for(const auto &directory:*list)if(accept(directory+"/"+name))return true;
   return false;
  };
  if(one(value,paths(runpath)))return value;
  // Bionic does not follow a second namespace link for the same lookup.
  // Dependencies start a new lookup from their actual provider namespace.
  const char *base=baseName(name);
  for(auto &link:value->links)if((link.all||link.names.contains(base))&&one(link.target,{}))return link.target;
  output[0]=0;return nullptr;
 }catch(const std::exception &error){lastError=error.what();return nullptr;}
}
extern "C" int klj_namespace_publish(KljLibraryNamespace *space,void *image,int global){
 try{
  if(!space||!image)return 0;std::lock_guard lock(space->owner->mutex);
  for(auto &member:space->members)if(member.image==image){member.global|=global!=0;return 1;}
  space->members.push_back({image,global!=0});return 1;
 }catch(const std::exception &error){lastError=error.what();return 0;}
}
extern "C" KljLibraryNamespace *klj_namespace_resolve_from(KljLibraryNamespace *space,const char *name,
 const char *requester,const char *runpath,unsigned elf_class,KljLibraryCandidate candidate,void *context,char *output,size_t capacity){
 try{
  std::string expanded=runpath?runpath:"";
  format_string(&expanded,{{"ORIGIN",libraryDirectory(requester?requester:"")},{"LIB",elf_class==1?"lib":"lib64"}});
  return klj_namespace_resolve(space,name,expanded.c_str(),candidate,context,output,capacity);
 }catch(const std::exception &error){lastError=error.what();return nullptr;}
}
extern "C" int klj_namespace_contains(KljLibraryNamespace *space,void *image){
 if(!space||!image)return 0;std::lock_guard lock(space->owner->mutex);
 return std::any_of(space->members.begin(),space->members.end(),[image](auto &member){return member.image==image;});
}
extern "C" int klj_namespace_global(KljLibraryNamespace *space,void *image){
 if(!space||!image)return 0;std::lock_guard lock(space->owner->mutex);
 return std::any_of(space->members.begin(),space->members.end(),[image](auto &member){return member.image==image&&member.global;});
}
extern "C" const char *klj_namespace_error(void){return lastError.empty()?nullptr:lastError.c_str();}
