#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <mach-o/loader.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Android's EGL service normally owns this persistent driver cache. The
// callbacks keep ANGLE's original program/shader serialization and key rules.
namespace klj_egl_cache {
constexpr size_t diskLimit=128*1024*1024,entryLimit=16*1024*1024,pendingLimit=32*1024*1024;
using Bytes=std::vector<unsigned char>;
inline std::string digest(const void *data,size_t size){
 unsigned char hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data,(CC_LONG)size,hash);
 static constexpr char hex[]="0123456789abcdef";std::string result(64,'0');
 for(size_t i=0;i<sizeof hash;++i){result[2*i]=hex[hash[i]>>4];result[2*i+1]=hex[hash[i]&15];}return result;
}
struct Header{uint64_t magic=UINT64_C(0x3145484341434c4b),size=0;unsigned char hash[CC_SHA256_DIGEST_LENGTH]{};};
inline bool transfer(int fd,void *data,size_t size,bool write){auto p=(unsigned char*)data;while(size){ssize_t n=write?::write(fd,p,size) : ::read(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;size-=n;}return true;}
class Cache:public std::enable_shared_from_this<Cache>{
 int directory=-1;std::string identity;
 std::mutex mutex;std::unordered_map<std::string,std::shared_ptr<Bytes>> pending;size_t pendingBytes=0,pendingJobs=0;
 dispatch_queue_t queue=dispatch_queue_create("maclauncher.android.egl-cache",DISPATCH_QUEUE_SERIAL);
 std::atomic<uint64_t> sequence{0};
 std::string name(const void *key,size_t size){std::string input=identity;input.append((const char*)key,size);return digest(input.data(),input.size())+".blob";}
 void prune(){
  int copy=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(copy<0)return;DIR *scan=fdopendir(copy);if(!scan){close(copy);return;}
  struct Entry{std::string name;uint64_t size;timespec modified;};std::vector<Entry> entries;uint64_t bytes=0;
  while(auto item=readdir(scan)){std::string n=item->d_name;if(n.size()!=69||n.substr(64)!=".blob")continue;struct stat st{};if(fstatat(directory,n.c_str(),&st,AT_SYMLINK_NOFOLLOW)||!S_ISREG(st.st_mode))continue;entries.push_back({n,(uint64_t)st.st_size,st.st_mtimespec});bytes+=st.st_size;}
  closedir(scan);if(bytes<=diskLimit&&entries.size()<=4096)return;
  std::sort(entries.begin(),entries.end(),[](auto&a,auto&b){return a.modified.tv_sec==b.modified.tv_sec?a.modified.tv_nsec<b.modified.tv_nsec:a.modified.tv_sec<b.modified.tv_sec;});
  size_t count=entries.size();for(auto &entry:entries){if(bytes<=diskLimit&&count<=4096)break;if(!unlinkat(directory,entry.name.c_str(),0)){bytes-=entry.size;--count;}}
 }
 void persist(const std::string &key,const std::shared_ptr<Bytes> &value){
  // Serialize disk maintenance across Android processes sharing this app data.
  // No file lock or disk write is taken by ANGLE's rendering thread.
  int lock=openat(directory,".lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
  if(lock>=0&&flock(lock,LOCK_EX)==0){
   auto temp=".pending-"+std::to_string(getpid())+"-"+std::to_string(sequence.fetch_add(1));
   int fd=openat(directory,temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
   if(fd>=0){Header header;header.size=value->size();CC_SHA256(value->data(),(CC_LONG)value->size(),header.hash);
    bool okay=transfer(fd,&header,sizeof header,true)&&transfer(fd,value->data(),value->size(),true);close(fd);
    if(okay)renameat(directory,temp.c_str(),directory,key.c_str());unlinkat(directory,temp.c_str(),0);prune();}
   flock(lock,LOCK_UN);
  }if(lock>=0)close(lock);
  std::lock_guard guard(mutex);pendingBytes-=value->size();--pendingJobs;auto at=pending.find(key);if(at!=pending.end()&&at->second==value)pending.erase(at);
 }
public:
 Cache(const char *root,std::string driver):identity(std::move(driver)){
  if(!root||!*root)return;int parent=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(parent<0)return;
  if(!mkdirat(parent,"egl-programs",0700)||errno==EEXIST)directory=openat(parent,"egl-programs",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(parent);
 }
 ~Cache(){if(directory>=0)close(directory);}
 bool ready()const{return directory>=0;}
 void set(const void *key,EGLsizeiANDROID keySize,const void *value,EGLsizeiANDROID size){
  if(!key||keySize<=0||keySize>4096||!value||size<=0||(size_t)size>entryLimit||directory<0)return;
  try{auto filename=name(key,keySize);std::shared_ptr<Bytes> bytes;
   {std::lock_guard guard(mutex);if(pendingBytes+(size_t)size>pendingLimit||pendingJobs>=256)return;
    bytes=std::make_shared<Bytes>((const unsigned char*)value,(const unsigned char*)value+size);
    pending[filename]=bytes;pendingBytes+=size;++pendingJobs;}
   auto self=shared_from_this();dispatch_async(queue,^{self->persist(filename,bytes);});
  }catch(...){/* A cache miss must never prevent shader compilation. */}
 }
 EGLsizeiANDROID get(const void *key,EGLsizeiANDROID keySize,void *value,EGLsizeiANDROID capacity){
  if(!key||keySize<=0||keySize>4096||capacity<0||directory<0)return 0;
  try{auto filename=name(key,keySize);{std::lock_guard guard(mutex);auto at=pending.find(filename);if(at!=pending.end()){auto &bytes=*at->second;if(value&&(size_t)capacity>=bytes.size())memcpy(value,bytes.data(),bytes.size());return bytes.size();}}
   int fd=openat(directory,filename.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
   struct stat st{};Header h;bool okay=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&transfer(fd,&h,sizeof h,false)&&h.magic==Header{}.magic&&h.size>0&&h.size<=entryLimit&&(uint64_t)st.st_size==sizeof h+h.size;
   if(!okay){close(fd);return 0;}if(!value||(uint64_t)capacity<h.size){close(fd);return h.size;}
   okay=transfer(fd,value,h.size,false);close(fd);unsigned char hash[CC_SHA256_DIGEST_LENGTH];if(okay){CC_SHA256(value,(CC_LONG)h.size,hash);okay=!memcmp(hash,h.hash,sizeof hash);}return okay?h.size:0;
  }catch(...){return 0;}
 }
 void drain(){dispatch_sync(queue,^{});}
};
inline std::shared_ptr<Cache> owner;
inline std::mutex ownerMutex;
inline void EGLAPIENTRY set(const void*k,EGLsizeiANDROID ks,const void*v,EGLsizeiANDROID vs){std::shared_ptr<Cache> cache;{std::lock_guard guard(ownerMutex);cache=owner;}if(cache)cache->set(k,ks,v,vs);}
inline EGLsizeiANDROID EGLAPIENTRY get(const void*k,EGLsizeiANDROID ks,void*v,EGLsizeiANDROID vs){std::shared_ptr<Cache> cache;{std::lock_guard guard(ownerMutex);cache=owner;}return cache?cache->get(k,ks,v,vs):0;}
inline std::string driverIdentity(void *address){
 std::string result="maclauncher-egl-cache-v1";Dl_info info{};
 if(dladdr(address,&info)&&info.dli_fbase){auto header=(const mach_header_64*)info.dli_fbase;if(header->magic==MH_MAGIC_64){auto begin=(const unsigned char*)(header+1),end=begin+header->sizeofcmds;for(auto p=begin;p+sizeof(load_command)<=end;){auto command=(const load_command*)p;if(command->cmdsize<sizeof(load_command)||command->cmdsize>(size_t)(end-p))break;if(command->cmd==LC_UUID&&command->cmdsize>=sizeof(uuid_command))result.append((const char*)((const uuid_command*)p)->uuid,16);p+=command->cmdsize;}}}
 char os[256];size_t size=sizeof os;if(!sysctlbyname("kern.osversion",os,&size,nullptr,0))result.append(os,size);return result;
}
inline void configure(const char *root,void *driver){auto next=std::make_shared<Cache>(root,driverIdentity(driver));std::lock_guard guard(ownerMutex);if(!owner&&next->ready())owner=std::move(next);}
inline bool install(EGLDisplay display,void *(*symbol)(const char*)){
 {std::lock_guard guard(ownerMutex);if(!owner)return false;}
 auto query=(PFNEGLQUERYSTRINGPROC)symbol("eglQueryString");auto install=(PFNEGLSETBLOBCACHEFUNCSANDROIDPROC)symbol("eglSetBlobCacheFuncsANDROID");
 if(!query||!install)return false;const char *extensions=query(display,EGL_EXTENSIONS);if(!extensions)return false;
 const char *match=strstr(extensions,"EGL_ANDROID_blob_cache");if(!match||(match!=extensions&&match[-1]!=' ')||(match[22]&&match[22]!=' '))return false;
 install(display,set,get);return true;
}
}
