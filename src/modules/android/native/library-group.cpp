#include "library-registry.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Locked {
 pthread_mutex_t *value;
 explicit Locked(pthread_mutex_t *value):value(value){pthread_mutex_lock(value);}
 ~Locked(){pthread_mutex_unlock(value);}
};
struct Node {
 std::string path,name;
 void *space,*image;
 bool fresh;
 std::vector<size_t> children;
};
struct Group {
 const KljLibraryGroupOperations &ops;
 KlImageRegistrySnapshot existing;
 char *error;size_t capacity;
 std::vector<std::unique_ptr<Node>> nodes;
 bool committed=false;
 ~Group(){if(!committed)for(auto i=nodes.rbegin();i!=nodes.rend();++i)if((*i)->fresh)ops.unmap(ops.context,(*i)->image);}
 [[noreturn]] void fail(const char *message){throw std::runtime_error(capacity&&error[0]?error:message);}
 size_t add(void *space,const char *path,const char *name){
  std::unique_ptr<char,decltype(&std::free)> canonical(realpath(path,nullptr),std::free);
  const char *key=canonical?canonical.get():path;
  for(size_t i=0;i<nodes.size();i++)if(nodes[i]->space==space&&nodes[i]->path==key)return i;
  void *image=nullptr;
  for(size_t i=0;i<existing.count;i++){
   void *candidate=existing.entries[i].img;const char *file=ops.path(ops.context,candidate);
   if(file&&key==std::string_view(file)&&ops.contains(ops.context,space,candidate)){image=candidate;break;}
  }
  auto node=std::make_unique<Node>(Node{key,name,space,image,image==nullptr,{}});
  if(!image){node->image=ops.map(ops.context,space,key,error,capacity);if(!node->image)fail("Native image mapping failed");}
  try{nodes.push_back(std::move(node));}catch(...){if(node&&node->fresh)ops.unmap(ops.context,node->image);throw;}
  return nodes.size()-1;
 }
 struct Discovery{Group &group;size_t index;};
 static int dependency(void *opaque,const char *name){
  auto &call=*static_cast<Discovery *>(opaque);auto &self=call.group;auto &node=*self.nodes[call.index];
  void *provider=node.space;char path[4096];
  int status=self.ops.resolve(self.ops.context,node.space,node.image,node.path.c_str(),name,&provider,path,sizeof path,self.error,self.capacity);
  if(!status)return 0;
  if(status==1)node.children.push_back(self.add(provider,path,name));
  return 1;
 }
 struct Lookup{Group &group;Node &node;};
 static void *symbol(void *opaque,const char *name){
  auto &call=*static_cast<Lookup *>(opaque);auto &self=call.group;
  // Original Bionic lookup order is the global group followed by the
  // breadth-first local load group. Platform imports remain the format's
  // final ABI resolver after this guest-symbol lookup.
  for(size_t i=0;i<self.existing.count;i++){
   void *image=self.existing.entries[i].img;
   if(self.ops.global(self.ops.context,call.node.space,image))
    if(void *value=self.ops.symbol(self.ops.context,image,name))return value;
  }
  for(auto &node:self.nodes)if(void *value=self.ops.symbol(self.ops.context,node->image,name))return value;
  if(self.ops.external_symbol)for(auto &node:self.nodes)
   if(void *value=self.ops.external_symbol(self.ops.context,node->image,name))return value;
  return nullptr;
 }
};
}
extern "C" void *klj_library_group_load(pthread_mutex_t *mutex,KlImageRegistry *registry,
 const KljLibraryGroupOperations *ops,void *space,const char *path,const char *name,
 int (*prepare)(void *,void *),void *context,char *error,size_t capacity){
 try{
  if(capacity)error[0]=0;
  Locked lock(mutex);Group group{*ops,klj_library_registry_snapshot(registry),error,capacity,{}};
  group.add(space,path,name);
  // The queue grows from original DT_NEEDED records. Mapping before
  // relocation permits cyclic dependencies without publishing partial images.
  for(size_t i=0;i<group.nodes.size();i++)if(group.nodes[i]->fresh){
   Group::Discovery discovery{group,i};
   if(!ops->dependencies(ops->context,group.nodes[i]->path.c_str(),Group::dependency,&discovery,error,capacity))group.fail("Native dependency discovery failed");
  }
  for(auto &node:group.nodes)if(node->fresh){
   if(ops->group_root)ops->group_root(ops->context,node->image,group.nodes.front()->image);
   Group::Lookup lookup{group,*node};
   if(!ops->relocate(ops->context,node->image,Group::symbol,&lookup,error,capacity))group.fail("Native relocation failed");
   if(ops->validate&&!ops->validate(ops->context,node->image,error,capacity))group.fail("Native image validation failed");
   std::vector<void *> children;for(size_t index:node->children)children.push_back(group.nodes[index]->image);
   if(!ops->dependencies_set(ops->context,node->image,children.data(),children.size()))group.fail("Native dependency ownership allocation failed");
  }
  void *result=group.nodes.front()->image;
  if(prepare&&!prepare(result,context))return nullptr;
  std::vector<KlImageRegistryEntry> entries;std::vector<void *> spaces;
  for(auto &node:group.nodes)if(node->fresh){entries.push_back({node->name.c_str(),node->image});spaces.push_back(node->space);}
  if(!entries.empty()&&!ops->publish(ops->context,entries.data(),spaces.data(),entries.size()))group.fail("Native image publication failed");
  group.committed=true;return result;
 }catch(const std::exception &failure){if(capacity)std::snprintf(error,capacity,"%s",failure.what());return nullptr;}
}
extern "C" void *klj_library_group_symbol(const KljLibraryGroupOperations *ops,
 void *space,void *root,void *skip_until,const char *name){
 // The original linker walks its retained DT_NEEDED edges, with one visited
 // set for cycles. These are the same images used during relocation.
 std::vector<void *> queue{root},visited;bool skip=skip_until!=nullptr;
 for(size_t index=0;index<queue.size();index++){
  void *image=queue[index];
  if(std::find(visited.begin(),visited.end(),image)!=visited.end())continue;
  visited.push_back(image);
  bool accessible=ops->contains(ops->context,space,image)!=0;
  if(skip)skip=image!=skip_until;
  else if(accessible){
   if(void *value=ops->symbol(ops->context,image,name))return value;
   if(ops->external_symbol)if(void *value=ops->external_symbol(ops->context,image,name))return value;
  }
  if(accessible&&ops->children)ops->children(ops->context,image,[](void *context,void *child){static_cast<std::vector<void *> *>(context)->push_back(child);},&queue);
 }
 return nullptr;
}
