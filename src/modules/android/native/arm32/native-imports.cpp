#include "native-imports.h"
#if __has_include(<ffi/ffi.h>)
#include <ffi/ffi.h>
#else
#include <ffi.h>
#endif
#if defined(__ANDROID__)
#include "../native-egl-trace.hpp"
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <memory_resource>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include "format-plan.h"
#include "../variadic-handlers.h"
#include "managed-exit.h"
#include "../proc-maps.h"
#include <sys/stat.h>
#include <sys/uio.h>
#include <zlib.h>

namespace maclauncher::arm32 {
namespace {
enum class Kind { Void, Signed, Unsigned, Float, Pointer, Record, Array, Function, Unsupported };
enum class FieldRole { Value, Padding, Mirror };
struct Field { unsigned type; int guestOffset, hostOffset, guestBits, hostBits; const char *name; FieldRole role; unsigned tagFamily; };
struct NativeField {unsigned type;int bits;};
struct Type {
  Kind kind; int guestSize, hostSize, guestAlign, hostAlign;
  bool constant, unionType; unsigned element, result;
  const unsigned *parameters; unsigned parameterCount;
  const Field *fields; unsigned fieldCount, count; bool variadic;
  const char *name; JniValue jni; Kind hostKind;
  const NativeField *hostFields;unsigned hostFieldCount,hostCount;bool hostUnion,packedBits;
  bool eglValue=false;
};
enum class CountOperation {Constant,Parameter,Add,Subtract,Multiply,Divide};
struct CountInstruction {CountOperation operation;int64_t value;};
struct CountPlan {unsigned parameter;const CountInstruction *instructions;unsigned size;};
struct Function { const char *name; unsigned type; const char *const *parameters; int format,firstVariadic; bool scanning=false; const CountPlan *counts=nullptr;unsigned countSize=0; };
struct Variable { const char *name; unsigned type; };
struct FileCommand { const char *name; unsigned guest,host; };
struct Interface {const char *name;unsigned family,type,identifierBytes;};
struct InterfaceQuery {unsigned type,family,identifier,output;};
struct TaggedRecord {unsigned family,guestTag,hostTag,type;};
#include "native-imports.inc"
#include "loader-abi.inc"
#include "signal-abi.inc"
enum class SyscallMode { Direct,Unavailable,OpenAt,Ptrace,Seek,Priority,Getcwd,ErrorNumber,
  AppendZero,Statfs,VectorOffset,Advice,SyncRange,SignalSize,SignalFd,Tls,CacheFlush };
struct Syscall {
  uint32_t number;const char *name,*service;SyscallMode mode;bool voidResult;unsigned count;
  struct Argument {unsigned offset,words;} arguments[7];
};
#include "syscalls.inc"
#include "futex-abi.inc"
#include "prctl-constants.h"
uint64_t read(const void *p, unsigned size) {
  uint64_t result = 0;
  if (size > sizeof result) throw std::runtime_error("Android ABI integer exceeds 64 bits");
  std::memcpy(&result, p, size); return result;
}
void write(void *p, uint64_t value, unsigned size) {
  if (size > sizeof value) throw std::runtime_error("Android ABI integer exceeds 64 bits");
  std::memcpy(p, &value, size);
}
uint64_t extend(uint64_t value, unsigned size, bool signedValue) {
  if (!signedValue || size >= 8) return value;
  const unsigned shift = 64 - size * 8;
  return uint64_t(int64_t(value << shift) >> shift);
}
[[noreturn]] void unsupported(const Type &type) {
  throw std::runtime_error(std::string("Android ARM32 native ABI adaptation is unavailable for ") + type.name);
}
}
struct Imports::Impl {
  struct CallFrame {
    Impl &owner;Thread &thread;CallFrame *previous;
    static thread_local CallFrame *current;
    CallFrame(Impl &owner,Thread &thread):owner(owner),thread(thread),previous(current){current=this;}
    ~CallFrame(){current=previous;}
  };
  Memory &memory;
  void *(*lookup)(const char *);
  size_t (*allocationCapacity)(const void *);
  std::recursive_mutex mutex;
  std::function<std::shared_ptr<Thread>()> threadFactory;
  std::function<uint64_t(Thread &,uint32_t,std::span<const Value>)> invokeCallback;
  std::function<uint64_t(JniValue,uint64_t,bool)> adaptJni;
  int (*threadExitRequested)(void) = nullptr;
  std::function<uint64_t(Thread &)> libcSyscall;
  struct Ffi { ffi_type type{}; std::vector<ffi_type *> elements; };
  std::map<unsigned, std::unique_ptr<Ffi>> aggregates;
  std::map<std::pair<std::string,void *>, uint32_t> gates;
  std::map<uint32_t,void *> nativeGateTargets;
  std::map<void *, uint32_t> allocations;
  struct Data {
    void *native;uint32_t guest;unsigned type;std::vector<uint8_t> previous,bytes,nativePrevious;
    // A native API may retain a caller-owned record's address between calls.
    // Keep its converted storage in this same representation owner; source
    // still belongs to the original guest allocation, rather than a new map.
    void *source=nullptr;std::vector<uint8_t> nativeStorage{};
    uint8_t *guestBytes(){return source?static_cast<uint8_t *>(source):bytes.data();}
  };
  std::list<Data> data;
  std::mutex procMutex;
  std::string procRoot;
  bool ownsProcRoot=false;
  struct Callback;
  std::map<std::pair<uint32_t,unsigned>,std::unique_ptr<Callback>> callbacks;
  explicit Impl(Memory &m, void *(*l)(const char *)) : memory(m), lookup(l),
    allocationCapacity(reinterpret_cast<size_t (*)(const void *)>(l("malloc_usable_size"))) {}
  ~Impl() {
    for (auto &[name,gate] : gates) { (void)name; memory.retireImport(gate); }
    for (const auto &view:data) if(!view.source)memory.unmap(view.guest);
    if(ownsProcRoot){
      unlink((procRoot+"/proc/self/maps").c_str());
      rmdir((procRoot+"/proc/self").c_str());rmdir((procRoot+"/proc").c_str());rmdir(procRoot.c_str());
    }
  }
  bool selfMaps(const char *path){
    if(!path||std::strncmp(path,"/proc/",6))return false;
    if(klj_proc_maps_self(path,0))return true;
    using ProcessId=int (*)();auto process=reinterpret_cast<ProcessId>(lookup("getpid"));
    return process&&klj_proc_maps_self(path,uint64_t(process()));
  }
  std::string mapsPath(){
    // Process/Thread ownership retains memory through this call. Its mapping
    // read guard is released before namespace lookup and filesystem I/O.
    const auto mappings=memory.mappings();std::vector<KljProcMapRegion> regions;
    regions.reserve(mappings.size());
    for(const auto &mapping:mappings)regions.push_back({mapping.start,mapping.end,0,0,0,
      ((mapping.permission&Memory::Read)?1u:0u)|((mapping.permission&Memory::Write)?2u:0u)|((mapping.permission&Memory::Execute)?4u:0u),mapping.shared,nullptr});
    auto image=reinterpret_cast<KljProcMapLookup>(lookup("klj_proc_maps_image"));
#if defined(__APPLE__)
    if(!image)image=reinterpret_cast<KljProcMapLookup>(dlsym(RTLD_DEFAULT,"klj_proc_maps_image"));
#endif
    struct Snapshot {const std::vector<KljProcMapRegion> &regions;KljProcMapLookup lookup;} snapshot{regions,image};
    std::lock_guard lock(procMutex);
    if(procRoot.empty()){
#if defined(__APPLE__)
      auto root=reinterpret_cast<const char *(*)()>(dlsym(RTLD_DEFAULT,"klj_proc_maps_root"));
      if(root)if(const char *path=root())procRoot=path;
#endif
      if(procRoot.empty()){
        const char *temporary=getenv("TMPDIR");if(!temporary||!*temporary)temporary="/tmp";
        std::string pattern=std::string(temporary)+"/klepton-proc32.XXXXXX";
        if(!mkdtemp(pattern.data()))throw std::system_error(errno,std::generic_category());
        procRoot=std::move(pattern);ownsProcRoot=true;
      }
    }
    using ProcessId=int (*)();auto process=reinterpret_cast<ProcessId>(lookup("getpid"));
    if(!process)throw std::system_error(ENOSYS,std::generic_category());
    char path[4096];
    if(!klj_proc_maps_publish(procRoot.c_str(),path,sizeof path,+[](FILE *file,void *opaque){
      const auto &value=*static_cast<Snapshot *>(opaque);
      return klj_proc_maps_format(file,value.regions.data(),value.regions.size(),value.lookup);
    },&snapshot,uint64_t(process())))throw std::system_error(errno,std::generic_category());
    return path;
  }
  int convertErrno(int value,bool toLinux) {
#if defined(__ANDROID__)
    // Both sides use original Bionic errno values when the ABI adapter is
    // compiled in ART's execution context. No Darwin service is called here.
    (void)toLinux;return value;
#else
    using Convert=int (*)(int);const char *name=toLinux?"kl_errno_to_linux":"kl_errno_from_linux";
    auto convert=reinterpret_cast<Convert>(dlsym(RTLD_DEFAULT,name));
    if(!convert){
      Dl_info location{};auto entry=lookup("syscall");
      if(entry&&dladdr(entry,&location)&&location.dli_fname){
        if(auto module=dlopen(location.dli_fname,RTLD_LAZY|RTLD_NOLOAD)){
          convert=reinterpret_cast<Convert>(dlsym(module,name));dlclose(module);
        }
      }
    }
    if(!convert)throw std::runtime_error("Shared Android errno owner is unavailable");
    return convert(value);
#endif
  }

  // Preserve addresses into converted native data, including arrays whose
  // element stride differs between ABIs. The service still receives its own
  // original object, rather than a transient copy with a different identity.
  uintptr_t dataPointer(uintptr_t pointer,bool toHost) {
    std::lock_guard lock(mutex);
    for(const auto &view:data) {
      const auto &t=types[view.type];const auto &element=t.kind==Kind::Array?types[t.element]:t;
      auto base=toHost?uintptr_t(view.guest):reinterpret_cast<uintptr_t>(view.native);
      auto size=toHost?t.guestSize:t.hostSize;
      if(pointer<base || pointer-base>=uintptr_t(size))continue;
      auto from=toHost?element.guestSize:element.hostSize,to=toHost?element.hostSize:element.guestSize;
      auto offset=pointer-base;
      if(from<=0 || offset%unsigned(from))continue;
      return (toHost?reinterpret_cast<uintptr_t>(view.native):uintptr_t(view.guest))+offset/unsigned(from)*unsigned(to);
    }
    return 0;
  }

  ffi_type *ffi(unsigned id) {
    const auto &t = types[id];
    switch (t.hostKind) {
    case Kind::Void: return &ffi_type_void;
    case Kind::Pointer: return &ffi_type_pointer;
    case Kind::Float:
      if (t.hostSize == 4) return &ffi_type_float;
      if (t.hostSize == 8) return &ffi_type_double;
#if defined(__ANDROID__)
      if (t.hostSize == sizeof(long double)) return &ffi_type_longdouble;
#endif
      unsupported(t); // Android binary128 needs its actual shared ABI owner.
    case Kind::Signed: case Kind::Unsigned: {
      const bool s = t.hostKind == Kind::Signed;
      switch (t.hostSize) {
      case 1: return s ? &ffi_type_sint8 : &ffi_type_uint8;
      case 2: return s ? &ffi_type_sint16 : &ffi_type_uint16;
      case 4: return s ? &ffi_type_sint32 : &ffi_type_uint32;
      case 8: return s ? &ffi_type_sint64 : &ffi_type_uint64;
      default: unsupported(t);
      }
    }
    case Kind::Record: case Kind::Array: {
      if (t.hostUnion || t.hostSize < 0) unsupported(t);
      std::lock_guard lock(mutex);
      auto &value = aggregates[id];
      if (!value) {
        value = std::make_unique<Ffi>(); value->type.type = FFI_TYPE_STRUCT;
        if (t.hostKind == Kind::Array) for (unsigned n=0;n<t.hostCount;n++) value->elements.push_back(ffi(t.element));
        else for (unsigned n=0;n<t.hostFieldCount;n++) {
          if (t.hostFields[n].bits) unsupported(t);
          value->elements.push_back(ffi(t.hostFields[n].type));
        }
        value->elements.push_back(nullptr); value->type.elements=value->elements.data();
      }
      return &value->type;
    }
    default: unsupported(t);
    }
  }
  static bool same(unsigned id) {
    const auto &t=types[id];
    if(t.guestSize!=t.hostSize || t.guestSize<0)return false;
    if(t.packedBits)return true;
    if(t.kind==Kind::Record&&t.unionType&&std::string_view(t.name)=="epoll_data_t")return true;
    if(t.kind==Kind::Pointer)return false;
    if(t.kind==Kind::Record) {
      for(unsigned n=0;n<t.fieldCount;n++)if(t.fields[n].guestOffset!=t.fields[n].hostOffset || t.fields[n].guestBits!=t.fields[n].hostBits || !same(t.fields[n].type))return false;
    }else if(t.kind==Kind::Array)return t.count==t.hostCount&&same(t.element);
    return t.kind!=Kind::Unsupported && t.kind!=Kind::Function;
  }
  uint32_t address(void *pointer, unsigned id, size_t extent=0,std::string_view origin={}) {
    if (!pointer) return 0;
    // Native publication and release share one transaction owner. A physical
    // allocator can reuse an address immediately after free/realloc returns.
    std::lock_guard lock(mutex);
    if(auto guest=dataPointer(reinterpret_cast<uintptr_t>(pointer),false))return uint32_t(guest);
    // macOS reserves the low 4 GiB in this 64-bit host. A value in that range
    // is an original guest address or opaque token, never native storage to
    // borrow. This also preserves tokens delivered through native callbacks.
    if(reinterpret_cast<uintptr_t>(pointer)<=UINT32_MAX)return uint32_t(reinterpret_cast<uintptr_t>(pointer));
    if (auto found=memory.addressOf(pointer)) return found;
    const auto &t=types[id];
    if (!extent && t.kind==Kind::Pointer) {
      const auto &p=types[t.element];
      // The original NDK exposes native resources such as FILE through a
      // private byte envelope. Its size describes the Android ABI, not a
      // serializable record or the native owner's macOS implementation.
      if(opaqueNative(p))extent=1;
      else if((p.kind==Kind::Pointer || p.kind==Kind::Record || p.kind==Kind::Array) && p.guestSize>0 && p.hostSize>0 && !same(t.element))
        return variable(pointer,t.element);
      else extent=std::max(1,p.hostSize);
      if ((p.kind==Kind::Signed || p.kind==Kind::Unsigned) && p.hostSize==1 && std::string_view(p.name).find("char")!=std::string_view::npos)
        extent=std::strlen(static_cast<const char *>(pointer))+1;
    }
    if (extent>UINT32_MAX) throw std::runtime_error("Android ARM32 native buffer exceeds its address space");
    return memory.borrow(pointer,uint32_t(std::max(size_t(1),extent)),t.eglValue?0:Memory::Read|Memory::Write,origin.empty()?t.name:origin);
  }
  static bool opaqueNative(const Type &type) {
    if(type.kind!=Kind::Record || type.fieldCount!=1 || std::string_view(type.fields[0].name)!="__private")return false;
    const auto &storage=types[type.fields[0].type];
    if(storage.kind!=Kind::Array)return false;
    const auto &element=types[storage.element];
    return element.guestSize==1 && element.hostSize==1;
  }
  static bool signalInfoType(const Type &type){
    auto name=std::string_view(type.name);if(name.starts_with("const "))name.remove_prefix(6);
    return type.kind==Kind::Record&&(name=="siginfo_t"||name=="struct siginfo");
  }
  struct Transfer {
    Impl &owner;
    std::string_view origin;
    struct View { uint32_t guest; unsigned type; std::vector<uint8_t> bytes; bool writable; unsigned count=1; void *native=nullptr; };
    std::vector<std::unique_ptr<View>> views;
    struct ByteWrite {uint32_t guest;size_t size;};
    std::vector<ByteWrite> byteWrites;
    std::map<uintptr_t,uint32_t> opaquePointers;
    explicit Transfer(Impl &o,std::string_view origin={}):owner(o),origin(origin){}
    ~Transfer(){for(auto&view:views)if(view->native&&view->guest)owner.memory.unmap(view->guest);}
    void *byteSpan(uint32_t pointer,size_t size,unsigned permission){
      if(!size)return reinterpret_cast<void *>(uintptr_t(pointer));
      void *bytes=owner.memory.translate(pointer,size,permission);
      if(permission&Memory::Write)byteWrites.push_back({pointer,size});
      return bytes;
    }
    void commitByteWrites(){
      if(byteWrites.empty())return;
      std::lock_guard lock(owner.mutex);
      for(auto &view:owner.data){
        if(types[view.type].constant)continue;
        const uint64_t end=uint64_t(view.guest)+view.previous.size();
        bool written=std::any_of(byteWrites.begin(),byteWrites.end(),[&](const ByteWrite &span){return uint64_t(span.guest)<end&&uint64_t(view.guest)<uint64_t(span.guest)+span.size;});
        if(written&&std::memcmp(view.guestBytes(),view.previous.data(),view.previous.size()))
          convert(view.type,view.guestBytes(),view.native,true);
      }
    }
    void *array(uint32_t pointer,unsigned element,unsigned count,bool opaqueElements=false) {
      if(!pointer)return nullptr;
      if(!count)return reinterpret_cast<void *>(uintptr_t(pointer));
      const auto &t=types[element];if(t.guestSize<=0 || t.hostSize<=0)unsupported(t);
      const size_t guestSize=size_t(t.guestSize)*count,hostSize=size_t(t.hostSize)*count;
      const auto *source=static_cast<const uint8_t *>(owner.memory.translate(pointer,guestSize,Memory::Read|(t.constant?0:Memory::Write)));
      if(same(element)&&!opaqueElements)return byteSpan(pointer,guestSize,Memory::Read|(t.constant?0:Memory::Write));
      auto view=std::make_unique<View>(View{pointer,element,std::vector<uint8_t>(hostSize),!t.constant,count});
      auto *raw=view.get();views.push_back(std::move(view));
      for(unsigned n=0;n<count;n++){
        auto *destination=raw->bytes.data()+size_t(n)*t.hostSize;
        if(opaqueElements)write(destination,read(source+size_t(n)*t.guestSize,unsigned(t.guestSize)),unsigned(t.hostSize));
        else convert(element,source+size_t(n)*t.guestSize,destination,true,true);
      }
      return raw->bytes.data();
    }
    bool same(unsigned id) {
      return Impl::same(id);
    }
    struct SignalField {unsigned type;size_t guest,host;};
    static std::optional<SignalField> signalField(unsigned id,std::string_view name){
      const auto&t=types[id];
      for(unsigned i=0;i<t.fieldCount;i++){
        const auto&f=t.fields[i];
        if(f.guestOffset<0||f.hostOffset<0||f.guestBits||f.hostBits)continue;
        if(std::string_view(f.name)==name)return SignalField{f.type,size_t(f.guestOffset/8),size_t(f.hostOffset/8)};
        if(std::string_view(f.name).starts_with("@anonymous"))if(auto child=signalField(f.type,name)){
          child->guest+=size_t(f.guestOffset/8);child->host+=size_t(f.hostOffset/8);return child;
        }
      }
      return std::nullopt;
    }
    static SignalField signalPath(unsigned id,std::string_view path){
      SignalField result{id,0,0};
      while(!path.empty()){
        auto dot=path.find('.');auto name=path.substr(0,dot);auto field=signalField(result.type,name);
        if(!field)unsupported(types[id]);
        result.type=field->type;result.guest+=field->guest;result.host+=field->host;
        if(dot==std::string_view::npos)break;path.remove_prefix(dot+1);
      }
      return result;
    }
    void signalInfo(unsigned id,const void*source,void*destination,bool toHost){
      const auto&abi=toHost?guestSignalAbi:hostSignalAbi;
      auto scalar=[&](const char*path){auto f=signalPath(id,path);const auto&t=types[f.type];return int32_t(read(static_cast<const uint8_t*>(source)+(toHost?f.guest:f.host),unsigned(toHost?t.guestSize:t.hostSize)));};
      const int signal=scalar("si_signo"),code=scalar("si_code");
      std::memset(destination,0,size_t(toHost?types[id].hostSize:types[id].guestSize));
      auto field=[&](const char*path,bool address=false,bool token=false){
        auto f=signalPath(id,path);const auto&t=types[f.type];
        auto*from=static_cast<const uint8_t*>(source)+(toHost?f.guest:f.host);
        auto*to=static_cast<uint8_t*>(destination)+(toHost?f.host:f.guest);
        if(address||token){
          uint64_t value=read(from,unsigned(toHost?t.guestSize:t.hostSize));
          // Fault addresses describe memory; they are never new readable
          // allocations. Recover an existing ABI alias if one is owned, and
          // preserve unmapped address bits without borrowing/dereferencing.
          // sigval instead carries an opaque application word (int or pointer).
          if(address&&value){
            if(toHost){try{value=uintptr_t(owner.memory.translate(uint32_t(value),1,0));}catch(const std::runtime_error&) {}}
            else if(auto alias=owner.memory.addressOf(reinterpret_cast<void*>(uintptr_t(value))))value=alias;
          }
          write(to,value,unsigned(toHost?t.hostSize:t.guestSize));
        }else convert(f.type,from,to,toHost,true);
      };
      field("si_signo");field("si_errno");field("si_code");
      // Linux selects siginfo payloads by the signal/code pair, including
      // positive legacy poll codes and the nested fault extensions. Offsets,
      // widths, alignment and discriminator values remain original-header data.
      enum class Payload {Kill,Timer,Realtime,Child,Fault,Poll,Syscall};
      Payload payload=Payload::Kill;
      if(code>abi.v_SI_USER&&code<abi.v_SI_KERNEL){
        bool specific=false;
        const struct {int signal,limit;Payload payload;} families[]={
          {abi.v_SIGILL,abi.v_NSIGILL,Payload::Fault},{abi.v_SIGFPE,abi.v_NSIGFPE,Payload::Fault},
          {abi.v_SIGSEGV,abi.v_NSIGSEGV,Payload::Fault},{abi.v_SIGBUS,abi.v_NSIGBUS,Payload::Fault},
          {abi.v_SIGTRAP,abi.v_NSIGTRAP,Payload::Fault},{abi.v_SIGCHLD,abi.v_NSIGCHLD,Payload::Child},
          {abi.v_SIGPOLL,abi.v_NSIGPOLL,Payload::Poll},{abi.v_SIGSYS,abi.v_NSIGSYS,Payload::Syscall}};
        for(const auto&family:families)if(signal==family.signal){specific=true;if(code<=family.limit)payload=family.payload;break;}
        if(!specific&&code<=abi.v_NSIGPOLL)payload=Payload::Poll;
      }else if(code==abi.v_SI_TIMER)payload=Payload::Timer;
      else if(code==abi.v_SI_SIGIO)payload=Payload::Poll;
      else if(code<0)payload=Payload::Realtime;
      switch(payload){
      case Payload::Kill:field("_sifields._kill._pid");field("_sifields._kill._uid");break;
      case Payload::Timer:field("_sifields._timer._tid");field("_sifields._timer._overrun");field("_sifields._timer._sigval.sival_ptr",false,true);field("_sifields._timer._sys_private");break;
      case Payload::Realtime:field("_sifields._rt._pid");field("_sifields._rt._uid");field("_sifields._rt._sigval.sival_ptr",false,true);break;
      case Payload::Child:for(auto path:{"_sifields._sigchld._pid","_sifields._sigchld._uid","_sifields._sigchld._status","_sifields._sigchld._utime","_sifields._sigchld._stime"})field(path);break;
      case Payload::Poll:field("_sifields._sigpoll._band");field("_sifields._sigpoll._fd");break;
      case Payload::Syscall:field("_sifields._sigsys._call_addr",true);field("_sifields._sigsys._syscall");field("_sifields._sigsys._arch");break;
      case Payload::Fault:
        field("_sifields._sigfault._addr",true);
        if(signal==abi.v_SIGBUS&&code>=abi.v_BUS_MCEERR_AR&&code<=abi.v_BUS_MCEERR_AO)field("_sifields._sigfault._addr_lsb");
        else if(signal==abi.v_SIGSEGV&&code==abi.v_SEGV_BNDERR){field("_sifields._sigfault._addr_bnd._lower",true);field("_sifields._sigfault._addr_bnd._upper",true);}
        else if(signal==abi.v_SIGSEGV&&code==abi.v_SEGV_PKUERR)field("_sifields._sigfault._addr_pkey._pkey");
        else if(signal==abi.v_SIGTRAP&&code==abi.v_TRAP_PERF)for(auto path:{"_sifields._sigfault._perf._data","_sifields._sigfault._perf._type","_sifields._sigfault._perf._flags"})field(path);
        break;
      }
    }
    void convert(unsigned id,const void *source,void *destination,bool toHost,bool nested=false,const char *unionMember=nullptr) {
      const auto &t=types[id]; int from=toHost?t.guestSize:t.hostSize,to=toHost?t.hostSize:t.guestSize;
      if(t.packedBits){std::memcpy(destination,source,size_t(from));return;}
      if(signalInfoType(t)){signalInfo(id,source,destination,toHost);return;}
      if(t.jni!=JniValue::None){
        if(!owner.adaptJni)throw std::runtime_error("JNI owner is unavailable for an Android native call");
        write(destination,owner.adaptJni(t.jni,read(source,unsigned(from)),toHost),unsigned(to));return;
      }
      switch(t.kind) {
      case Kind::Void:return;
      case Kind::Signed:case Kind::Unsigned:
        // Android declares pthread_t as long, but its value is an opaque native
        // thread identity. Translate its representation instead of truncating
        // the canonical shared owner's pointer.
        if(std::string_view(t.name)=="pthread_t") {
          auto value=read(source,unsigned(from));
          if(toHost)write(destination,value?uintptr_t(owner.memory.translate(uint32_t(value),1,0)):0,unsigned(to));
          else write(destination,value?owner.address(reinterpret_cast<void *>(uintptr_t(value)),id,1,origin):0,unsigned(to));
          return;
        }
        write(destination,extend(read(source,unsigned(from)),unsigned(from),(toHost?t.kind:t.hostKind)==Kind::Signed),unsigned(to));return;
      case Kind::Float:
        if(from==to && from<=8){std::memcpy(destination,source,size_t(from));return;}
        unsupported(t);
      case Kind::Pointer: {
        const auto &p=types[t.element];
        if(toHost) {
          uint32_t pointer=uint32_t(read(source,4)); void *native=nullptr;
          if(pointer) {
            bool opaque=false;
            if(nested)try{owner.memory.translate(pointer,1,0);}catch(const std::runtime_error&){opaque=true;}
            if(std::string_view(t.name)=="locale_t"&&pointer==UINT32_MAX){native=reinterpret_cast<void *>(UINTPTR_MAX);}
            else if(p.kind==Kind::Function&&(pointer==1||pointer==UINT32_MAX)){
              native=reinterpret_cast<void *>(pointer==UINT32_MAX?UINTPTR_MAX:uintptr_t(pointer));
            }
            else if(opaque){
              // Record fields and pointer output slots may contain arbitrary
              // incoming bits. Preserve unmapped values without interpreting
              // them as readable buffers; if the native owner leaves them
              // untouched, copy-back preserves the original representation.
              native=reinterpret_cast<void *>(uintptr_t(pointer));
              opaquePointers[uintptr_t(native)]=pointer;
            }
            else if(auto original=owner.dataPointer(pointer,true))native=reinterpret_cast<void *>(original);
            else if(t.eglValue){
              // EGL validates its opaque values. Existing aliases still use
              // their exact native identity, without reading resource bytes;
              // unmapped tokens/labels retain their original integer value.
              try{native=owner.memory.translate(pointer,1,0);}
              catch(const std::runtime_error&){
                if(owner.memory.mappingDescription(pointer)!="unmapped")throw;
                native=reinterpret_cast<void *>(uintptr_t(pointer));
              }
            }
            else if(p.kind==Kind::Function) native=owner.callback(pointer,t.element);
            // Klepton's existing synchronization owners use the object's
            // stable address and Bionic's first state word, for both ABIs.
            // Passing that address avoids introducing a second mutex registry.
            else if(p.kind==Kind::Record && (std::string_view(p.name)=="pthread_mutex_t" || std::string_view(p.name)=="pthread_cond_t" || std::string_view(p.name)=="pthread_rwlock_t"))
              native=owner.memory.translate(pointer,4,Memory::Read|Memory::Write);
            else if(opaqueNative(p))native=owner.memory.translate(pointer,1,Memory::Read);
            else if(p.guestSize<=0 || p.kind==Kind::Void || same(t.element)) native=owner.memory.translate(pointer,size_t(std::max(1,p.guestSize)),Memory::Read);
            else {
              auto found=std::find_if(views.begin(),views.end(),[&](const auto &v){return v->guest==pointer && v->type==t.element;});
              if(found!=views.end())native=(*found)->bytes.data();
              else {
                auto view=std::make_unique<View>(View{pointer,t.element,std::vector<uint8_t>(size_t(p.hostSize)),!p.constant});
                auto *raw=view.get();views.push_back(std::move(view));native=raw->bytes.data();
                convert(t.element,owner.memory.translate(pointer,size_t(p.guestSize),Memory::Read),native,true,true);
              }
            }
          }
          std::memcpy(destination,&native,sizeof native);
        } else {
          void *native=nullptr;std::memcpy(&native,source,sizeof native);
          uint32_t guest=0;
          if(native) {
            if(std::string_view(t.name)=="locale_t"&&reinterpret_cast<uintptr_t>(native)==UINTPTR_MAX)guest=UINT32_MAX;
            if(p.kind==Kind::Function){
              auto value=reinterpret_cast<uintptr_t>(native);
              if(value==1 || value==UINTPTR_MAX)guest=uint32_t(value);
              else guest=owner.nativeFunctionAddress(native,t.element);
            }
            if(auto value=opaquePointers.find(uintptr_t(native));value!=opaquePointers.end())guest=value->second;
            for(const auto &view:views)if(native==view->bytes.data()){guest=view->guest;break;}
            if(!guest&&signalInfoType(p)){
              // A delivered siginfo lives on the native signal frame. Keep its
              // ABI view with this invocation, never in the persistent native
              // resource table which is synchronized after the frame returns.
              auto view=std::make_unique<View>(View{0,t.element,std::vector<uint8_t>(size_t(p.guestSize)),!p.constant,1,native});
              convert(t.element,native,view->bytes.data(),false,true);
              view->guest=owner.memory.borrow(view->bytes.data(),uint32_t(p.guestSize),Memory::Read|Memory::Write,p.name);
              guest=view->guest;views.push_back(std::move(view));
            }
            if(!guest)guest=owner.address(native,id,0,origin);
          }
          write(destination,guest,4);
        }
        return;
      }
      case Kind::Array:
        // A fixed-capacity array can have different cardinalities in the two
        // original declarations. Convert corresponding elements only; unused
        // destination storage retains its original value. Semantic bitmaps
        // use their complete packed representation above instead of widening
        // each storage word and corrupting the native allocation.
        for(unsigned i=0;i<std::min(t.count,t.hostCount);i++)convert(t.element,static_cast<const uint8_t *>(source)+i*unsigned(toHost?types[t.element].guestSize:types[t.element].hostSize),static_cast<uint8_t *>(destination)+i*unsigned(toHost?types[t.element].hostSize:types[t.element].guestSize),toHost,true);
        return;
      case Kind::Record:
        if(t.unionType) {
          // epoll returns the application's data word verbatim. Even its
          // pointer spelling is an opaque identifier, never a native buffer
          // for the kernel/pollable owner to dereference.
          if(std::string_view(t.name)=="epoll_data_t")unionMember="u64";
          if(unionMember){
            for(unsigned i=0;i<t.fieldCount;i++)if(std::string_view(t.fields[i].name)==unionMember){
              const auto &f=t.fields[i];
              convert(f.type,static_cast<const uint8_t *>(source)+(toHost?f.guestOffset:f.hostOffset)/8,static_cast<uint8_t *>(destination)+(toHost?f.hostOffset:f.guestOffset)/8,toHost,true);
              return;
            }
          }
          if(same(id)){std::memcpy(destination,source,size_t(from));return;} unsupported(t);
        }
        // The original sigaction flags select the active anonymous-union
        // callback signature. Both layouts come from Clang; this only selects
        // the declared member before the ordinary callback-width adaptation.
        if(std::string_view(t.name).find("struct sigaction")!=std::string_view::npos){
          for(unsigned i=0;i<t.fieldCount;i++)if(std::string_view(t.fields[i].name)=="sa_flags"){
            const auto &f=t.fields[i];auto flags=read(static_cast<const uint8_t *>(source)+(toHost?f.guestOffset:f.hostOffset)/8,4);
            unionMember=(flags&4)?"sa_sigaction":"sa_handler";
          }
        }
        for(unsigned i=0;i<t.fieldCount;i++) {
          const auto &f=t.fields[i];int a=toHost?f.guestOffset:f.hostOffset,b=toHost?f.hostOffset:f.guestOffset;
          if(f.role==FieldRole::Padding){
            if(!toHost)std::memset(static_cast<uint8_t *>(destination)+f.guestOffset/8,0,size_t(types[f.type].guestSize));
            continue;
          }
          if(f.role==FieldRole::Mirror&&toHost)continue;
          if(a<0 || b<0 || f.guestBits || f.hostBits) unsupported(t);
          const auto *fieldSource=static_cast<const uint8_t *>(source)+a/8;
          unsigned fieldType=f.type;
          if(f.tagFamily){
            auto pointer=read(fieldSource,unsigned(toHost?types[f.type].guestSize:types[f.type].hostSize));
            if(pointer){
              const void *record=toHost?owner.memory.translate(uint32_t(pointer),4,Memory::Read):reinterpret_cast<const void *>(uintptr_t(pointer));
              auto tag=read(record,4);
              for(const auto &candidate:taggedRecords)if(candidate.family==f.tagFamily&&tag==(toHost?candidate.guestTag:candidate.hostTag)){
                fieldType=candidate.type;break;
              }
            }
          }
          convert(fieldType,fieldSource,static_cast<uint8_t *>(destination)+b/8,toHost,true,unionMember);
        }
        return;
      default:unsupported(t);
      }
    }
    void finish() {
      // Convert nested views before parent pointer fields; storage remains live
      // until all original native calls and copy-back operations are complete.
      for(size_t i=views.size();i>0;i--) {
        auto &v=*views[i-1];if(v.writable) {
          if(v.native){convert(v.type,v.bytes.data(),v.native,true);continue;}
          const auto &t=types[v.type];auto *destination=static_cast<uint8_t *>(owner.memory.translate(v.guest,size_t(t.guestSize)*v.count,Memory::Write));
          for(unsigned n=0;n<v.count;n++)convert(v.type,v.bytes.data()+size_t(n)*t.hostSize,destination+size_t(n)*t.guestSize,false);
        }
      }
    }
  };
  struct Signature {
    ffi_cif cif{};std::vector<ffi_type *> arguments;void *target=nullptr;unsigned fixed=0;bool formatted=false,wordVariadic=false,nativeVariadic=false;
    std::vector<unsigned> wordOffsets;unsigned fixedWords=0;
    std::vector<unsigned> graphicsBindings;
    void (*graphicsQuery)(unsigned,int *)=nullptr;
    Signature(Impl &owner,unsigned id,const Function *function=nullptr,void *native=nullptr):target(native) {
      const auto &t=types[id];if(t.kind!=Kind::Function)unsupported(t);
      formatted=function&&function->format>=0;fixed=t.parameterCount;
      // These public C interfaces have one optional promoted GP operand. The
      // original native handler retains all command/flags and file semantics.
      const std::string_view name=function?function->name:"";
      wordVariadic=t.variadic&&!formatted&&(name=="open"||name=="open64"||name=="fcntl"||name=="fcntl64");
      if(formatted||wordVariadic){
        if(t.variadic){
#if defined(__ANDROID__)
          // Original Bionic exports the actual variadic function. libffi
          // applies its AAPCS64 calling convention to the same argument plan.
          nativeVariadic=true;
#else
          using Lookup=const KLVariadicHandler *(*)(const void *);
          auto handler=reinterpret_cast<Lookup>(dlsym(RTLD_DEFAULT,"klj_variadic_handler"));
          const KLVariadicHandler *service=handler?handler(native):nullptr;
          if(!service){Dl_info location{};if(dladdr(native,&location)&&location.dli_fname){void *module=dlopen(location.dli_fname,RTLD_LAZY|RTLD_NOLOAD);if(module){handler=reinterpret_cast<Lookup>(dlsym(module,"klj_variadic_handler"));service=handler?handler(native):nullptr;dlclose(module);}}}
          if(!service||service->fixed!=fixed)throw std::runtime_error("Shared native variadic handler is unavailable for "+std::string(function->name));
          target=service->target;
#endif
        }else{if(!fixed)unsupported(t);--fixed;}
      }else if(t.variadic)unsupported(t);
      const auto &result=types[t.result];
      fixedWords=result.kind==Kind::Record&&result.guestSize>4?1:0;
      for(unsigned n=0;n<fixed;n++){
        const auto &parameter=types[t.parameters[n]];
        if(parameter.guestSize<0)unsupported(parameter);
        if(parameter.guestAlign>=8&&fixedWords%2)++fixedWords;
        wordOffsets.push_back(fixedWords);
        fixedWords+=unsigned(parameter.guestSize+3)/4;
        arguments.push_back(owner.ffi(t.parameters[n]));
        graphicsBindings.push_back(function&&parameter.kind==Kind::Pointer?glBinding(*function,n):0);
      }
      if(std::any_of(graphicsBindings.begin(),graphicsBindings.end(),[](unsigned binding){return binding!=0;})){
        // This prepared signature already retains its native callable. Resolve
        // the matching context query from the same caller's provider once;
        // the actual binding remains a live query on every invocation.
        const int incomingError=errno;
        graphicsQuery=reinterpret_cast<decltype(graphicsQuery)>(owner.lookup("glGetIntegerv"));
        errno=incomingError;
        if(!graphicsQuery)throw std::runtime_error("Android OpenGL context query is unavailable");
      }
      if((formatted||wordVariadic)&&!nativeVariadic)arguments.push_back(&ffi_type_pointer);
      if(ffi_prep_cif(&cif,FFI_DEFAULT_ABI,unsigned(arguments.size()),owner.ffi(t.result),arguments.data())!=FFI_OK)unsupported(t);
    }
  };
  uint32_t variable(void *native,unsigned type) {
    std::lock_guard lock(mutex);
    if(auto existing=dataPointer(reinterpret_cast<uintptr_t>(native),false))return uint32_t(existing);
    Transfer transfer(*this);const auto &t=types[type];
    if(t.guestSize<=0 || t.hostSize<=0)unsupported(t);
    if(transfer.same(type))return memory.borrow(native,uint32_t(t.guestSize),Memory::Read|(t.constant?0:Memory::Write),t.name);
    // Publish the representation identity before following any pointer field.
    // Native interface tables and ordinary records may refer back to their
    // own object; the same view closes that cycle without copying ownership.
    auto view=data.insert(data.end(),Data{native,0,type,std::vector<uint8_t>(size_t(t.guestSize)),std::vector<uint8_t>(size_t(t.guestSize)),std::vector<uint8_t>(size_t(t.hostSize))});
    try {
      // The view owns its exact original guest extent. Page reservation is an
      // address-space detail, never permission to access adjacent ABI fields.
      view->guest=memory.borrow(view->bytes.data(),uint32_t(t.guestSize),Memory::Read|Memory::Write,t.name);
      auto *bytes=view->bytes.data();
      transfer.convert(type,native,bytes,false);
      std::memcpy(view->previous.data(),bytes,size_t(t.guestSize));
      std::memcpy(view->nativePrevious.data(),native,size_t(t.hostSize));return view->guest;
    }catch(...){
      // Recursive views belong to this same publication. A failed conversion
      // must not leave aliases to partially initialized representation storage.
      for(auto it=view;it!=data.end();++it)memory.unmap(it->guest);
      data.erase(view,data.end());throw;
    }
  }
  void retainRecord(uint32_t guest,unsigned type){
    if(!guest||same(type))return;
    std::lock_guard lock(mutex);
    if(dataPointer(guest,true))return;
    const auto&t=types[type];if(t.kind!=Kind::Record||t.guestSize<=0||t.hostSize<=0)unsupported(t);
    auto *source=memory.translate(guest,size_t(t.guestSize),Memory::Read|Memory::Write);
    auto view=data.insert(data.end(),Data{nullptr,guest,type,std::vector<uint8_t>(size_t(t.guestSize)),{},std::vector<uint8_t>(size_t(t.hostSize)),source,std::vector<uint8_t>(size_t(t.hostSize))});
    view->native=view->nativeStorage.data();
    try{
      Transfer transfer(*this);transfer.convert(type,source,view->native,true);
      std::memcpy(view->previous.data(),source,size_t(t.guestSize));
      std::memcpy(view->nativePrevious.data(),view->native,size_t(t.hostSize));
    }catch(...){
      for(auto it=view;it!=data.end();++it)if(!it->source)memory.unmap(it->guest);
      data.erase(view,data.end());throw;
    }
  }
  void releaseRecord(uint32_t guest){
    std::lock_guard lock(mutex);
    for(auto it=data.begin();it!=data.end();++it)if(it->guest==guest&&it->source){data.erase(it);return;}
  }
  void *recordSource(uint32_t guest){
    std::lock_guard lock(mutex);
    for(auto&view:data)if(view.source&&guest>=view.guest&&uint64_t(guest)-view.guest<view.previous.size())
      return static_cast<uint8_t*>(view.source)+(guest-view.guest);
    return nullptr;
  }
  void synchronizeData(bool toHost) {
    std::lock_guard lock(mutex);Transfer transfer(*this);
    for(auto &view:data) {
      // Data owns these exact representation bytes until retirement under
      // this mutex. Its guest mapping borrows the same stable buffer; native
      // synchronization needs neither address translation nor another owner.
      auto *bytes=view.guestBytes();
      if(toHost) {
        if(!types[view.type].constant && std::memcmp(bytes,view.previous.data(),view.previous.size())){
          transfer.convert(view.type,bytes,view.native,true);
          std::memcpy(view.nativePrevious.data(),view.native,view.nativePrevious.size());
          std::memcpy(view.previous.data(),bytes,view.previous.size());
        }
      } else if(std::memcmp(view.native,view.nativePrevious.data(),view.nativePrevious.size())||std::memcmp(bytes,view.previous.data(),view.previous.size())){
        transfer.convert(view.type,view.native,bytes,false);
        std::memcpy(view.previous.data(),bytes,view.previous.size());
        std::memcpy(view.nativePrevious.data(),view.native,view.nativePrevious.size());
      }
    }
  }
  struct NativeSpan {const void *bytes;size_t size;};
  static bool contains(const NativeSpan &span,const void *pointer){
    const auto base=reinterpret_cast<uintptr_t>(span.bytes),value=reinterpret_cast<uintptr_t>(pointer);
    return value>=base&&value-base<span.size;
  }
  void retireResources(std::span<const NativeSpan> spans){
    std::lock_guard lock(mutex);
    auto retiring=[&](const void *pointer){return std::any_of(spans.begin(),spans.end(),[&](const auto &span){return contains(span,pointer);});};
    for(auto it=data.begin();it!=data.end();)if(retiring(it->native)||(it->source&&retiring(it->source))){
      if(!it->source)memory.unmap(it->guest);it=data.erase(it);
    }else ++it;
    for(const auto &span:spans){
      auto it=allocations.lower_bound(const_cast<void *>(span.bytes));
      while(it!=allocations.end()&&contains(span,it->first))it=allocations.erase(it);
      memory.retireNative(span.bytes,span.size);
    }
  }
  size_t allocationExtent(const void *pointer,size_t requested=0)const{
    size_t extent=std::max(size_t(1),requested);
    if(allocationCapacity)extent=std::max(extent,allocationCapacity(pointer));
    // An allocator's usable-size metadata is not a VM protection boundary.
    // Scudo can report the requested byte count in a fundamentally aligned block,
    // although the remaining bytes of that alignment unit share the same
    // mapped page. Preserve that native addressability, without rounding to a
    // page or changing the original allocator/query. Requiring the actual
    // pointer alignment also keeps a differently aligned allocation exact.
    constexpr size_t alignment=nativeAllocationAlignment;
    if(reinterpret_cast<uintptr_t>(pointer)%alignment==0&&extent<=SIZE_MAX-(alignment-1))
      extent=(extent+alignment-1)&~(alignment-1);
    return extent;
  }
  uint32_t publishAllocation(void *pointer,unsigned type,size_t requested,std::string_view origin){
    if(!pointer)return 0;
    std::lock_guard lock(mutex);
    const size_t extent=allocationExtent(pointer,requested);
    // A successful allocator result establishes a new lifetime. Native NDK
    // owners can have released earlier objects internally; their stale opaque
    // aliases must not override this allocation's real capacity and ownership.
    const NativeSpan fresh{pointer,extent};retireResources(std::span(&fresh,1));
    auto guest=address(pointer,type,extent,origin);allocations[pointer]=guest;return guest;
  }
  // A deallocator's ownership contract selects the root, while original Clang
  // types describe its complete graph. This is representation retirement only;
  // the original native owner still frees its resources and determines errors.
  void ownedPointers(const void *pointer,unsigned type,std::vector<NativeSpan> &spans,std::set<std::pair<const void *,unsigned>> &seen){
    if(!pointer||reinterpret_cast<uintptr_t>(pointer)<=UINT32_MAX||!seen.emplace(pointer,type).second)return;
    spans.push_back({pointer,size_t(std::max(1,types[type].hostSize))});ownedValue(pointer,type,spans,seen);
  }
  void ownedValue(const void *storage,unsigned type,std::vector<NativeSpan> &spans,std::set<std::pair<const void *,unsigned>> &seen){
    const auto &t=types[type];
    if(t.kind==Kind::Pointer){
      if(types[t.element].kind==Kind::Function)return;
      void *pointer=nullptr;std::memcpy(&pointer,storage,sizeof pointer);ownedPointers(pointer,t.element,spans,seen);
    }else if(t.kind==Kind::Record&&!t.unionType&&!opaqueNative(t)){
      for(unsigned i=0;i<t.fieldCount;i++){
        const auto &field=t.fields[i];
        if(field.hostOffset>=0&&!field.hostBits)ownedValue(static_cast<const uint8_t *>(storage)+field.hostOffset/8,field.type,spans,seen);
      }
    }else if(t.kind==Kind::Array&&types[t.element].hostSize>0){
      for(unsigned i=0;i<t.count;i++)ownedValue(static_cast<const uint8_t *>(storage)+size_t(i)*types[t.element].hostSize,t.element,spans,seen);
    }
  }
  void retireResolver(void *pointer,unsigned type){
    std::vector<NativeSpan> spans;std::set<std::pair<const void *,unsigned>> seen;
    ownedPointers(pointer,type,spans,seen);
    // The original resolver contract supplies lengths for sockaddr storage and
    // owns a NUL-terminated canonical name. Clang supplies every field offset
    // and width; do not infer ownership for arbitrary pointer payloads.
    const auto &record=types[type];
    auto field=[&](const char *name){return std::find_if(record.fields,record.fields+record.fieldCount,[&](const auto &f){return std::string_view(f.name)==name;});};
    auto address=field("ai_addr"),length=field("ai_addrlen"),canonical=field("ai_canonname");
    if(address==record.fields+record.fieldCount||length==record.fields+record.fieldCount||canonical==record.fields+record.fieldCount)unsupported(record);
    for(const auto &[node,nodeType]:seen)if(nodeType==type){
      const auto *bytes=static_cast<const uint8_t *>(node);void *value=nullptr;
      std::memcpy(&value,bytes+address->hostOffset/8,sizeof value);
      auto size=read(bytes+length->hostOffset/8,unsigned(types[length->type].hostSize));
      if(value&&size)spans.push_back({value,size_t(size)});
      std::memcpy(&value,bytes+canonical->hostOffset/8,sizeof value);
      if(value)spans.push_back({value,std::strlen(static_cast<const char *>(value))+1});
    }
    retireResources(spans);
  }
  uint32_t resolveMemory(std::string_view name) {
    const bool mapping=name=="mmap" || name=="mmap64" || name=="__mmap2";
    if(!mapping && name!="munmap" && name!="mprotect")return 0;
    auto native=lookup(mapping?"mmap":std::string(name).c_str());
    if(!native)return 0;
    std::lock_guard lock(mutex);auto key=std::make_pair(std::string(name),native);
    if(auto found=gates.find(key);found!=gates.end())return found->second;
    auto gate=memory.import([this,name=std::string(name),native,mapping](Thread &thread)->uint64_t{
      const auto address=thread.argument(0),length=thread.argument(1);
      if((!length && name!="mprotect") || uint64_t(length)+4095>UINT32_MAX){errno=EINVAL;return UINT32_MAX;}
      const auto size=(length+4095)&~4095u;
      auto permissions=[](unsigned protection){return ((protection&1)?Memory::Read:0u)|((protection&2)?Memory::Write:0u)|((protection&4)?Memory::Execute:0u);};
      try {
        if(!mapping){
          if(name=="munmap")memory.unmap(address,size);
          else memory.protect(address,size,permissions(thread.argument(2)));
          return 0;
        }
        const auto protection=thread.argument(2),flags=thread.argument(3);
        const bool noReplace=(flags&0x100000)!=0,fixed=noReplace || (flags&0x10);
        if(fixed && (!address || address%4096)){errno=EINVAL;return UINT32_MAX;}
        int64_t offset=name=="__mmap2"?int64_t(uint64_t(thread.argument(5))*4096):
          name=="mmap64"?int64_t(uint64_t(thread.argument(6))|(uint64_t(thread.argument(7))<<32)):int64_t(int32_t(thread.argument(5)));
        if(offset<0 || offset%4096){errno=EINVAL;return UINT32_MAX;}
        // The common mapping owner consumes byte offsets and host pages.
        // Retain its allocation while guest 4 KiB regions split or retire.
        const uint64_t hostPage=uint64_t(sysconf(_SC_PAGESIZE));
        const auto delta=uint64_t(offset)%hostPage;
        const int64_t aligned=offset-int64_t(delta);
        const size_t extent=size_t((uint64_t(size)+delta+hostPage-1)/hostPage*hostPage);
        using Map=void *(*)(void *,size_t,int,int,int,int64_t);
        using Unmap=int (*)(void *,size_t);using Protect=int (*)(void *,size_t,int);
        auto unmap=reinterpret_cast<Unmap>(lookup("munmap"));auto protect=reinterpret_cast<Protect>(lookup("mprotect"));
        if(!unmap || !protect)throw std::runtime_error("Shared Android mapping owner is unavailable");
        auto map=reinterpret_cast<Map>(native);int nativeFlags=int(flags&~(0x10u|0x100000u));
        void *base=map(nullptr,extent,3,nativeFlags,int(thread.argument(4)),aligned);bool writable=true;
        if(base==reinterpret_cast<void *>(UINTPTR_MAX) && !(protection&2)){
          base=map(nullptr,extent,1,nativeFlags,int(thread.argument(4)),aligned);writable=false;
        }
        if(base==reinterpret_cast<void *>(UINTPTR_MAX))return UINT32_MAX;
        struct Mapping {void *base;size_t extent;Unmap unmap;Protect protect;bool writable;~Mapping(){unmap(base,extent);}};
        std::shared_ptr<Mapping> owner;
        try{owner=std::make_shared<Mapping>(base,extent,unmap,protect,writable);}
        catch(...){unmap(base,extent);throw;}
        auto prepare=[owner](unsigned permission){
          if((permission&Memory::Write) && !owner->writable){
            if(owner->protect(owner->base,owner->extent,3))throw std::system_error(errno,std::generic_category());
            owner->writable=true;
          }
        };
        return memory.mapNative(static_cast<uint8_t *>(base)+delta,size,permissions(protection),owner,address,fixed,noReplace,std::move(prepare),(flags&1)!=0);
      }catch(const std::system_error &error){errno=error.code().value();}
       catch(const std::bad_alloc &){errno=ENOMEM;}
       catch(const std::runtime_error &error){
         const std::string_view reason=error.what();
         errno=reason.find("Occupied")!=reason.npos?EEXIST:reason.find("exhausted")!=reason.npos || (name=="mprotect" && reason.find("Unmapped")!=reason.npos)?ENOMEM:EINVAL;
       }
      return UINT32_MAX;
    });
    gates.emplace(std::move(key),gate);return gate;
  }
  uint32_t resolveNative(std::string_view name,void *native) {
    if(!native)return 0;
#if defined(__ANDROID__)
    // The JVM's EGL supplier already exposes these same optional diagnostics.
    // ART retains original Bionic GLES providers; bind the shared observer at
    // its native-width entry before applying the ordinary width conversion.
    if(name.starts_with("gl"))if(auto observed=klj_egl_trace::lookup(std::string(name).c_str(),lookup))native=observed;
#endif
    std::lock_guard lock(mutex);auto key=std::make_pair(std::string(name),native);
    if(auto found=gates.find(key);found!=gates.end())return found->second;
    if(name=="syscall"){
      auto gate=memory.import([this](Thread &thread){return libcSyscall(thread);});
      gates.emplace(std::move(key),gate);return gate;
    }
    if(name=="ptrace"){
      auto gate=memory.import([native](Thread &thread)->uint64_t{
        // Bionic's exported variadic declaration fronts the same fixed
        // kernel operands. Tracer addresses stay opaque in the process owner.
        using Trace=long (*)(int,int,void *,void *);
        return uint32_t(reinterpret_cast<Trace>(native)(int(thread.argument(0)),int(thread.argument(1)),reinterpret_cast<void *>(uintptr_t(thread.argument(2))),reinterpret_cast<void *>(uintptr_t(thread.argument(3)))));
      });gates.emplace(std::move(key),gate);return gate;
    }
    if(name=="prctl"){
      auto gate=memory.import([this,native](Thread &thread)->uint64_t{
        auto operation=thread.argument(0);uintptr_t first=thread.argument(1);
        try{
          if(operation==PR_GET_NAME)first=reinterpret_cast<uintptr_t>(memory.translate(uint32_t(first),16,Memory::Write));
          else if(operation==PR_SET_NAME){
            // The Linux name API reads at most fifteen bytes, stopping at NUL.
            // Validate precisely that span rather than treating scalar control
            // operands as guest pointers.
            unsigned length=0;for(;length<15;length++)if(!*static_cast<const char *>(memory.translate(uint32_t(first)+length,1,Memory::Read))){++length;break;}
            first=reinterpret_cast<uintptr_t>(memory.translate(uint32_t(first),length,Memory::Read));
          }
        }catch(const Memory::Fault &){errno=EFAULT;return UINT32_MAX;}
        using Control=int (*)(int,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
        return uint32_t(reinterpret_cast<Control>(native)(int(operation),first,thread.argument(2),thread.argument(3),thread.argument(4)));
      });gates.emplace(std::move(key),gate);return gate;
    }
    auto data=std::find_if(std::begin(variables),std::end(variables),[&](const auto &v){return name==v.name;});
    if(data!=std::end(variables)){auto address=variable(native,data->type);gates.emplace(std::move(key),address);return address;}
    auto entry=std::find_if(std::begin(functions),std::end(functions),[&](const auto &f){return name==f.name;});
    if(entry==std::end(functions))return 0;
    return bindFunction(*entry,native,std::move(key));
  }
  uint32_t bindFunction(Function entry,void *native,std::pair<std::string,void *> key){
    std::lock_guard lock(mutex);
    if(auto found=gates.find(key);found!=gates.end())return found->second;
    // Interface-table methods have a type identity rather than an exported
    // symbol name. Bind their original spans to the existing conversion plan.
    for(const auto &plan:typedCounts)if(plan.type==entry.type){entry.counts=plan.counts;entry.countSize=plan.size;break;}
    struct Prepared {std::once_flag once;std::unique_ptr<Signature> value;};
    auto signature=std::make_shared<Prepared>();
    auto gate=memory.import([this,entry,native,signature](Thread &thread){
      try{std::call_once(signature->once,[&]{std::lock_guard guard(mutex);signature->value=std::make_unique<Signature>(*this,entry.type,&entry,native);});return invoke(entry,native,*signature->value,thread);}
      catch(const Memory::Fault& error){throw Memory::Fault(callFailure(entry,thread,error));}
      catch(const std::exception& error){throw std::runtime_error(callFailure(entry,thread,error));}
    });
    gates.emplace(std::move(key),gate);nativeGateTargets.emplace(gate,native);return gate;
  }
  static std::string callFailure(const Function &entry,Thread &thread,const std::exception &error){
    std::string result=std::string("ARM32 native call ")+entry.name+": "+error.what();
    // Read the interrupted guest state only on the existing error path.
    // Diagnostic transport failure must not replace the original exception.
    try{
      std::array<uint32_t,4> words{};thread.arguments(0,words);
      char context[128];std::snprintf(context,sizeof context,
        " [lr=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x]",
        thread.returnAddress(),words[0],words[1],words[2],words[3]);
      result+=context;
    }catch(...){ }
    return result;
  }
  uint32_t nativeFunctionAddress(void *native,unsigned type){
    std::lock_guard lock(mutex);
    // Reverse callbacks retain their original ARM32 identity. A genuinely
    // native function receives an executable typed import gate, never a
    // borrowed data page containing ARM64 machine instructions.
    if(auto address=callbackAddress(native))return address;
    Function function{types[type].name,type,nullptr,-1,-1};
    return bindFunction(function,native,{"@function:"+std::to_string(type),native});
  }
  struct Callback {
    Impl &owner;uint32_t address;unsigned type;Signature signature;ffi_closure *closure=nullptr;void *entry=nullptr;
    Callback(Impl &o,uint32_t a,unsigned t):owner(o),address(a),type(t),signature(o,t) {
      closure=static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure),&entry));
      if(!closure || ffi_prep_closure_loc(closure,&signature.cif,invoke,this,entry)!=FFI_OK)throw std::runtime_error("Android native callback ABI allocation failed");
    }
    ~Callback(){if(closure)ffi_closure_free(closure);}
    static void invoke(ffi_cif *,void *result,void **arguments,void *opaque) {
      auto &self=*static_cast<Callback *>(opaque);const auto &t=types[self.type];
      auto thread=self.owner.threadFactory?self.owner.threadFactory():std::make_shared<Thread>(self.owner.memory);
      Transfer transfer(self.owner,t.name);std::vector<Value> values;
      for(unsigned i=0;i<t.parameterCount;i++) {
        const auto &p=types[t.parameters[i]];if(p.guestSize>8)unsupported(p);
        uint64_t bits=0;transfer.convert(t.parameters[i],arguments[i],&bits,false);
        if(p.kind==Kind::Signed&&p.guestSize<4)bits=uint32_t(extend(bits,unsigned(p.guestSize),true));
        values.push_back({bits,unsigned((p.guestSize+3)/4),unsigned(p.guestAlign)});
      }
      try {
        auto bits=self.owner.invokeCallback?self.owner.invokeCallback(*thread,self.address,values):thread->call(self.address,values);
        transfer.convert(t.result,&bits,result,true);transfer.finish();
      } catch (const ManagedThreadExit &) {
        // A host callback returns through its original ABI. Its enclosing
        // import observes the same pending exit after ffi_call returns.
        if (types[t.result].hostSize)
          std::memset(result,0,size_t(types[t.result].hostSize));
        transfer.finish();
      } catch (const std::exception &failure) {
        char entry[64];std::snprintf(entry,sizeof entry," callback=0x%08x",self.address);
        std::string context=entry;
        for(unsigned i=0;i<std::min(size_t(8),values.size());++i){
          const auto &parameter=types[t.parameters[i]];
          char argument[256];const auto native=parameter.hostSize>0&&parameter.hostSize<=8?read(arguments[i],unsigned(parameter.hostSize)):0;
          std::snprintf(argument,sizeof argument," arg%u(%.96s)=0x%llx native=0x%llx",i,parameter.name,
            static_cast<unsigned long long>(values[i].bits),static_cast<unsigned long long>(native));
          context+=argument;
          if(parameter.kind==Kind::Pointer&&types[parameter.element].kind!=Kind::Function)
            context+=" ["+self.owner.memory.mappingDescription(uint32_t(values[i].bits))+"]";
          if(context.size()>1536){context.resize(1536);break;}
        }
        throw std::runtime_error(std::string(failure.what())+context);
      }
    }
  };
  void *callback(uint32_t address,unsigned type) {
    std::lock_guard lock(mutex);
    // Returning a native function to its original owner must preserve its
    // physical identity, not wrap its ARM32 gate in a second callback closure.
    if(auto found=nativeGateTargets.find(address);found!=nativeGateTargets.end())return found->second;
    auto &entry=callbacks[{address,type}];
    if(!entry)entry=std::make_unique<Callback>(*this,address,type);return entry->entry;
  }
  uint32_t callbackAddress(void *entry) {
    std::lock_guard lock(mutex);
    for(const auto &[key,value]:callbacks)if(value->entry==entry)return key.first;
    return 0;
  }
  // OpenGL pointer arguments are buffer offsets only when the original GL
  // context has the corresponding buffer bound. The context remains the sole
  // owner; this adapter merely changes a 32-bit offset/pointer representation.
  static unsigned glBinding(const Function &function,unsigned index) {
    std::string_view name=function.name,parameter=function.parameters?function.parameters[index]:"";
    if(!name.starts_with("gl"))return 0;
    unsigned binding=0;
    if(parameter=="indices" && name.find("Draw")!=name.npos)binding=0x8895;
    else if(parameter=="pointer" && name.find("Pointer")!=name.npos)binding=0x8894;
    else if(parameter=="indirect")binding=0x8f43;
    else if((parameter=="pixels" || parameter=="data") && (name.find("TexImage")!=name.npos || name.find("TexSubImage")!=name.npos))binding=0x88ef;
    else if(parameter=="pixels" && name.find("ReadPixels")!=name.npos)binding=0x88ed;
    return binding;
  }
  static bool glOffset(const Signature &signature,unsigned index) {
    const auto binding=signature.graphicsBindings[index];if(!binding)return false;
    int value=0;signature.graphicsQuery(binding,&value);return value!=0;
  }
  static std::optional<size_t> cardinality(const CountPlan &plan,const Type &signature,std::span<const uint64_t> words){
    std::vector<int64_t> stack;stack.reserve(plan.size);
    for(unsigned i=0;i<plan.size;i++){
      const auto &instruction=plan.instructions[i];
      if(instruction.operation==CountOperation::Constant){stack.push_back(instruction.value);continue;}
      if(instruction.operation==CountOperation::Parameter){
        auto index=unsigned(instruction.value);const auto &type=types[signature.parameters[index]];
        const int64_t value=int64_t(extend(words[index],unsigned(type.guestSize),type.kind==Kind::Signed));
        if(value<0)return std::nullopt;stack.push_back(value);continue;
      }
      const auto right=stack.back();stack.pop_back();const auto left=stack.back();stack.pop_back();
      __int128 result=0;
      switch(instruction.operation){
      case CountOperation::Add:result=__int128(left)+right;break;
      case CountOperation::Subtract:result=__int128(left)-right;break;
      case CountOperation::Multiply:result=__int128(left)*right;break;
      case CountOperation::Divide:if(!right)return std::nullopt;result=left/right;break;
      default:break;
      }
      if(result<0)return std::nullopt;
      if(result>UINT32_MAX)throw Memory::Fault("Android graphics array crosses its address space");
      stack.push_back(int64_t(result));
    }
    return size_t(stack.back());
  }
  uint64_t invoke(const Function &function,void *native,Signature &signature,Thread &thread) {
    CallFrame frame(*this,thread);
    const int incomingError=errno;
    // These ABI temporaries belong to this invocation, including nested native
    // callbacks. Standard monotonic storage avoids one allocator round-trip
    // per scalar argument while retaining ordinary heap fallback for large
    // calls; no thread-local pool or state survives the call.
    alignas(std::max_align_t) std::array<std::byte,512> local;
    std::pmr::monotonic_buffer_resource scratch(local.data(),local.size());
    auto bytes=[&](size_t size){
      // Original byte vectors used malloc's full alignment. Preserve it for
      // libffi aggregates, pointers and long doubles, rather than requesting
      // only uint8_t alignment from the arena.
      auto *data=static_cast<uint8_t*>(scratch.allocate(size,alignof(std::max_align_t)));
      std::memset(data,0,size);return std::span<uint8_t>(data,size);
    };
    const auto &t=types[function.type];Transfer transfer(*this,function.name);
    const std::string_view name=function.name;
    const bool reallocating=name=="realloc"||name=="reallocarray";
    std::unique_lock<std::recursive_mutex> resourceLease(mutex,std::defer_lock);
    if(name=="free"||reallocating||name=="freeaddrinfo")resourceLease.lock();
    std::pmr::vector<void *> pointers(signature.arguments.size(),&scratch);
    std::pmr::vector<uint64_t> words(t.parameterCount,&scratch);unsigned cursor=signature.fixedWords;
    std::pmr::vector<uint32_t> argumentWords(signature.fixedWords,&scratch);
    thread.arguments(0,argumentWords);
    const InterfaceQuery *interfaceQuery=nullptr;
    for(const auto &query:interfaceQueries)if(query.type==function.type){interfaceQuery=&query;break;}
    void *interfaceOutput=nullptr,*interfaceInitial=nullptr;
    unsigned interfaceType=0;
    const auto &resultType=types[t.result];
    uint32_t aggregateResult=0;
    if(resultType.kind==Kind::Record && resultType.guestSize>4)aggregateResult=argumentWords[0];
    for(unsigned i=0;i<signature.fixed;i++) {
      const auto &p=types[t.parameters[i]];if(p.guestSize<0)unsupported(p);
      // The original AAPCS32 word vector already owns these exact argument
      // bytes. Conversion reads them without another per-parameter copy.
      const auto *guest=p.guestSize?argumentWords.data()+signature.wordOffsets[i]:nullptr;
      words[i]=p.guestSize?read(guest,unsigned(std::min(8,p.guestSize))):0;
    }
    // zlib's public initialization ABI includes sizeof(z_stream) as a scalar
    // in addition to the stream pointer. The ordinary record converter owns
    // its LP32/LP64 layout; the scalar must describe that same converted
    // record. Invalid guest sizes retain zlib's original VERSION_ERROR and
    // leave the stream untouched, even if they equal the native record size.
    const Type *zlibStream=nullptr;
    if(name=="deflateInit_"||name=="deflateInit2_"||name=="inflateInit_"||
       name=="inflateInit2_"||name=="inflateBackInit_"){
      const auto &pointer=types[t.parameters[0]];
      if(pointer.kind!=Kind::Pointer)unsupported(pointer);
      zlibStream=&types[pointer.element];
      if(zlibStream->kind!=Kind::Record)unsupported(*zlibStream);
      if(int32_t(words[signature.fixed-1])!=zlibStream->guestSize){
        errno=incomingError;return uint32_t(Z_VERSION_ERROR);
      }
    }
    const bool zlibCopy=name=="inflateCopy"||name=="deflateCopy";
    const bool zlibEnd=name=="inflateEnd"||name=="deflateEnd"||name=="inflateBackEnd";
    const Type *zlibRecord=zlibStream;
    if(zlibCopy||zlibEnd)zlibRecord=&types[types[t.parameters[0]].element];
    // zlib retains this exact z_stream address in its internal state. A
    // per-call width-conversion temporary passes initialization but makes a
    // later inflate/deflate fail the original owner's identity check.
    if(zlibStream||zlibCopy)retainRecord(uint32_t(words[0]),types[t.parameters[0]].element);
    if(name=="process_vm_readv"||name=="process_vm_writev"){
      // Both vector arrays are original iovec records. Their bases are byte
      // addresses, rather than native-object handles, and remote addresses of
      // this process use the same ARM32 memory owner as local addresses.
      if(words[5]||words[2]>syscallMaxIov||words[4]>syscallMaxIov){errno=EINVAL;return UINT32_MAX;}
      const bool writing=name=="process_vm_writev";
      auto pid=reinterpret_cast<int (*)()>(lookup("getpid"));
      const bool self=pid&&int32_t(words[0])==pid();
      std::array<std::vector<iovec>,2> vectors;
      try{for(unsigned which=0;which<2;which++){
        unsigned index=which?3:1,count=unsigned(words[index+1]);
        const auto &record=types[types[t.parameters[index]].element];
        auto base=std::find_if(record.fields,record.fields+record.fieldCount,[](const Field &field){return std::string_view(field.name)=="iov_base";});
        auto length=std::find_if(record.fields,record.fields+record.fieldCount,[](const Field &field){return std::string_view(field.name)=="iov_len";});
        if(base==record.fields+record.fieldCount||length==record.fields+record.fieldCount)unsupported(record);
        const auto *array=count?static_cast<const uint8_t *>(memory.translate(uint32_t(words[index]),size_t(record.guestSize)*count,Memory::Read)):nullptr;
        size_t total=0;vectors[which].reserve(count);
        for(unsigned n=0;n<count;n++){
          const auto *entry=array+size_t(n)*record.guestSize;
          auto address=uint32_t(read(entry+base->guestOffset/8,4)),size=uint32_t(read(entry+length->guestOffset/8,4));
          if(size>size_t(INT32_MAX)-total){errno=EINVAL;return UINT32_MAX;}total+=size;
          void *pointer=reinterpret_cast<void *>(uintptr_t(address));
          if(!which||self)try{pointer=transfer.byteSpan(address,size,(which?writing:!writing)?Memory::Write:Memory::Read);}
          catch(const Memory::Fault &){pointer=reinterpret_cast<void *>(uintptr_t(1));}
          vectors[which].push_back({pointer,size});
        }
      }}catch(const Memory::Fault &){errno=EFAULT;return UINT32_MAX;}
      using Operation=ssize_t (*)(pid_t,const iovec *,unsigned long,const iovec *,unsigned long,unsigned long);
      errno=incomingError;
      auto answer=reinterpret_cast<Operation>(native)(int32_t(words[0]),vectors[0].data(),vectors[0].size(),vectors[1].data(),vectors[1].size(),0);
      int error=errno;transfer.commitByteWrites();errno=error;return uint64_t(answer);
    }
    for(unsigned i=0;i<signature.fixed;i++) {
      const auto &p=types[t.parameters[i]];const auto *guest=p.guestSize?argumentWords.data()+signature.wordOffsets[i]:nullptr;
      pointers[i]=bytes(size_t(std::max(16,p.hostSize))).data();
      // Byte contracts select the guest representation before ordinary typed
      // pointer conversion. In particular, an empty span must not first read
      // a byte, and a fill destination needs write permission, not read access.
      size_t byteCount=0;unsigned bytePermission=0;
      if(p.kind==Kind::Pointer){
        if(i<2&&(name=="memcpy"||name=="memmove"||name=="memcmp")){
          byteCount=size_t(words[2]);bytePermission=i==0&&name!="memcmp"?Memory::Write:Memory::Read;
        }else if(i==0&&(name=="memset"||name=="memset_explicit")){
          byteCount=size_t(words[2]);bytePermission=Memory::Write;
        }else if(i==0&&name=="__bionic_bzero"){
          byteCount=size_t(words[1]);bytePermission=Memory::Write;
        }else if(i==0&&(name=="qsort"||name=="qsort_r")){
          byteCount=size_t(words[1])*size_t(words[2]);bytePermission=Memory::Read|Memory::Write;
        }else if(i<2&&name=="bsearch"){
          byteCount=i==0?size_t(words[3]):size_t(words[2])*size_t(words[3]);bytePermission=Memory::Read;
        }else if(i==1&&(name=="read"||name=="pread"||name=="pread64"||name=="write"||name=="pwrite"||name=="pwrite64"||name=="sendto"||name=="recvfrom")){
          byteCount=size_t(words[2]);bytePermission=name=="read"||name=="pread"||name=="pread64"||name=="recvfrom"?Memory::Write:Memory::Read;
        }
      }
      std::optional<size_t> count;
      for(unsigned n=0;n<function.countSize;n++)if(function.counts[n].parameter==i)count=cardinality(function.counts[n],t,words);
      const bool graphicsOffset=p.kind==Kind::Pointer&&glOffset(signature,i);
      if(bytePermission){
        void *bytes=transfer.byteSpan(uint32_t(words[i]),byteCount,bytePermission);std::memcpy(pointers[i],&bytes,sizeof bytes);
      }
      else if(count&&p.kind==Kind::Pointer&&(!graphicsOffset||types[p.element].kind==Kind::Pointer)){
        const auto &element=types[p.element];void *array;
        if(!words[i])array=nullptr;
        else if(element.kind==Kind::Void)array=transfer.byteSpan(uint32_t(words[i]),*count,element.constant?Memory::Read:Memory::Read|Memory::Write);
        else array=transfer.array(uint32_t(words[i]),p.element,unsigned(*count),graphicsOffset);
        std::memcpy(pointers[i],&array,sizeof array);
      }
      else if(i==1 && (name=="readv" || name=="writev" || name=="preadv" || name=="pwritev" || name=="preadv64" || name=="pwritev64" || name=="preadv64v2" || name=="pwritev64v2")){
        const auto count=thread.argument(2);const auto &element=types[p.element];
        // iovec is an array, and each nested base denotes its declared byte
        // span. The same conversion serves imported functions and raw SVC.
        auto base=std::find_if(element.fields,element.fields+element.fieldCount,[](const Field &f){return std::string_view(f.name)=="iov_base";});
        auto length=std::find_if(element.fields,element.fields+element.fieldCount,[](const Field &f){return std::string_view(f.name)=="iov_len";});
        if(base==element.fields+element.fieldCount || length==element.fields+element.fieldCount)unsupported(element);
        if(int32_t(count)<0 || count>syscallMaxIov){errno=EINVAL;return UINT32_MAX;}
        auto vectors=static_cast<uint8_t *>(transfer.array(uint32_t(words[i]),p.element,count));
        for(unsigned n=0;n<count;n++){
          const auto *vector=static_cast<const uint8_t *>(memory.translate(uint32_t(words[i])+n*unsigned(element.guestSize),unsigned(element.guestSize),Memory::Read));
          auto address=uint32_t(read(vector+base->guestOffset/8,4));auto size=uint32_t(read(vector+length->guestOffset/8,4));
          // iov_base is a byte range, even when its address identifies a
          // converted pointer/table. Keep the original vector's typed layout,
          // but bind its data operand to the actual guest representation.
          void *bytes=transfer.byteSpan(address,size,name.find("read")!=name.npos?Memory::Write:Memory::Read);
          std::memcpy(vectors+size_t(n)*element.hostSize+base->hostOffset/8,&bytes,sizeof bytes);
        }
        std::memcpy(pointers[i],&vectors,sizeof vectors);
      }
      else if(i==1&&name.starts_with("epoll_")&&p.kind==Kind::Pointer&&std::string_view(types[p.element].name)=="struct epoll_event"&&words[i]&&int32_t(thread.argument(2))>0){
        const auto count=thread.argument(2);const auto&element=types[p.element];
        void *events=transfer.same(p.element)?memory.translate(uint32_t(words[i]),size_t(element.guestSize)*count,Memory::Read|Memory::Write):transfer.array(uint32_t(words[i]),p.element,count);
        std::memcpy(pointers[i],&events,sizeof events);
      }
      else if((name=="getopt" || name=="getopt_long" || name=="getopt_long_only") && i==1) {
        auto argv=transfer.array(uint32_t(words[i]),p.element,unsigned(words[0])+1);std::memcpy(pointers[i],&argv,sizeof argv);
      }
      else if((name=="getopt_long" || name=="getopt_long_only") && i==3 && words[i]) {
        const auto &option=types[p.element];unsigned count=0;
        do {++count;}while(read(memory.translate(uint32_t(words[i])+count*unsigned(option.guestSize),4,Memory::Read),4));
        auto options=transfer.array(uint32_t(words[i]),p.element,count+1);std::memcpy(pointers[i],&options,sizeof options);
      }
      else if((name.starts_with("exec")||name=="fexecve")&&p.kind==Kind::Pointer&&types[p.element].kind==Kind::Pointer&&words[i]){
        // argv/envp are terminated pointer arrays. Convert the complete
        // original list, rather than giving the process owner a one-entry
        // temporary view with no terminator.
        uint64_t count=0;for(;;){auto address=uint64_t(uint32_t(words[i]))+count*4;if(address>UINT32_MAX-3)throw Memory::Fault("Android process pointer array crosses its address space");if(!read(memory.translate(uint32_t(address),4,Memory::Read),4))break;++count;}
        auto values=transfer.array(uint32_t(words[i]),p.element,unsigned(count+1));std::memcpy(pointers[i],&values,sizeof values);
      }
      else if((name=="dladdr" && i==0) || (name=="pthread_setspecific" && i==1) || graphicsOffset) {
        // dladdr queries an address in the common image registry; it never
        // dereferences that address. Preserve even an unmapped guest value so
        // the original owner can return normal absence. GL offsets similarly
        // identify storage owned by the current context. Thread-specific data
        // is an opaque token retained by the existing pthread owner; it may
        // identify PROT_NONE storage or no mapped memory at all.
        auto address=uintptr_t(words[i]);std::memcpy(pointers[i],&address,sizeof address);
      }
      else if((name=="dlsym" || name=="dlvsym") && i==0 && (words[i]==guest_RTLD_DEFAULT || words[i]==guest_RTLD_NEXT)) {
        auto handle=uintptr_t(words[i]==guest_RTLD_DEFAULT?host_RTLD_DEFAULT:host_RTLD_NEXT);std::memcpy(pointers[i],&handle,sizeof handle);
      }
      else if((name=="dlopen" || name=="android_dlopen_ext") && i==1){
        uint32_t known=0,flags=0;for(const auto &flag:loaderFlags){known|=flag.guest;if(flag.guest&&(uint32_t(words[i])&flag.guest)==flag.guest)flags|=flag.host;}
        flags|=uint32_t(words[i])&~known;
        // Bionic LP32 expresses RTLD_NOW by the absence of RTLD_LAZY.
        if(!(uint32_t(words[i])&guest_RTLD_LAZY))flags|=host_RTLD_NOW;
        std::memcpy(pointers[i],&flags,sizeof flags);
      }
      else if(i==0&&(name=="free"||reallocating)&&recordSource(uint32_t(words[i]))){
        auto source=recordSource(uint32_t(words[i]));std::memcpy(pointers[i],&source,sizeof source);
      }
      else if(zlibStream&&i+1==signature.fixed)
        write(pointers[i],unsigned(zlibStream->hostSize),unsigned(p.hostSize));
      else transfer.convert(t.parameters[i],guest,pointers[i],true);
    }
    // Both C varargs and ARM32 va_list are promoted AAPCS32 words. Construct
    // the existing ARM64 service's stack-backed va_list after width conversion.
    struct Va64 {void *stack,*general,*vector;int generalOffset,vectorOffset;} va{};
    std::vector<uint8_t> variadic;
    std::vector<ffi_type *> variadicTypes;
    std::vector<size_t> variadicOffsets;
    if(signature.formatted){
      std::vector<unsigned> plan;
      const char *format=*static_cast<const char **>(pointers[unsigned(function.format)]);
      int planned=0;
      if(format)planned=function.scanning?klj_arm32_scan_plan(format,&plan,[](void *opaque,unsigned type){static_cast<std::vector<unsigned>*>(opaque)->push_back(scanTypes[type]);return 0;}):klj_arm32_format_plan0(format,&plan,[](void *opaque,unsigned type){static_cast<std::vector<unsigned>*>(opaque)->push_back(formatTypes[type]);return 0;});
      if(planned)throw std::runtime_error("Android format argument plan could not be read");
      uint32_t list=t.variadic?0:thread.argument(cursor);
      size_t extent=0;for(auto id:plan){const auto &p=types[id];size_t alignment=std::max(8,p.hostAlign);extent=(extent+alignment-1)&~(alignment-1);extent+=(size_t(p.hostSize)+7)&~size_t(7);}
      variadic.resize(extent);size_t offset=0;
      for(auto id:plan){
        const auto &p=types[id];if(p.guestSize<=0||p.guestSize>8)unsupported(p);
        uint64_t bits=0;
        if(t.variadic){if(p.guestAlign>=8&&cursor%2)cursor++;for(unsigned n=0;n<unsigned(p.guestSize+3)/4;n++)bits|=uint64_t(thread.argument(cursor++))<<(32*n);}
        else{list=(list+unsigned(p.guestAlign)-1)&~unsigned(p.guestAlign-1);bits=read(memory.translate(list,p.guestSize,Memory::Read),p.guestSize);list+=unsigned(p.guestSize);}
        size_t alignment=std::max(8,p.hostAlign);offset=(offset+alignment-1)&~(alignment-1);
        if(signature.nativeVariadic){variadicTypes.push_back(ffi(id));variadicOffsets.push_back(offset);}
        if(p.kind==Kind::Float&&p.guestSize==8&&p.hostSize==16){
          // Android ARM32 long double is binary64; ARM64 is binary128. Widen
          // the representation exactly, including subnormal and NaN payloads.
          using Binary128=unsigned __int128;
          uint64_t fraction=bits&0xfffffffffffffULL,exponent=(bits>>52)&2047;Binary128 wide=Binary128(bits>>63)<<127;
          if(exponent==2047)wide|=(Binary128(32767)<<112)|(Binary128(fraction)<<60);
          else if(exponent)wide|=(Binary128(exponent+16383-1023)<<112)|(Binary128(fraction)<<60);
          else if(fraction){unsigned top=63-unsigned(__builtin_clzll(fraction));wide|=(Binary128(16383-1074+top)<<112)|(Binary128(fraction-(1ULL<<top))<<(112-top));}
          std::memcpy(variadic.data()+offset,&wide,16);
        }else if(p.kind==Kind::Pointer&&types[p.element].kind==Kind::Void){
          // printf's void* argument is a displayed guest address, not a buffer
          // for the native formatter to dereference.
          uintptr_t address=uint32_t(bits);std::memcpy(variadic.data()+offset,&address,sizeof address);
        }else transfer.convert(id,&bits,variadic.data()+offset,true);
        offset+=(size_t(p.hostSize)+7)&~size_t(7);
      }
    }else if(signature.wordVariadic){
      variadic.resize(8);uint32_t word=thread.argument(cursor);uint64_t operand=word;
      if(name=="fcntl"||name=="fcntl64"){
        for(const auto&command:fileCommands){
          const std::string_view operation=command.name;
          const bool ofd=operation=="F_OFD_GETLK"||operation=="F_OFD_SETLK"||operation=="F_OFD_SETLKW";
          const bool lock=operation=="F_GETLK"||operation=="F_SETLK"||operation=="F_SETLKW"||ofd;
          const bool lock64=operation=="F_GETLK64"||operation=="F_SETLK64"||operation=="F_SETLKW64";
          if(command.guest!=words[1]||(!lock&&!lock64))continue;
          const auto type=fileTypes[lock64||ofd||name=="fcntl64"?1:0];transfer.convert(type,&word,&operand,true);
          write(pointers[1],command.host,unsigned(types[t.parameters[1]].hostSize));break;
        }
      }
      std::memcpy(variadic.data(),&operand,sizeof operand);
      if(signature.nativeVariadic){variadicTypes.push_back(&ffi_type_uint64);variadicOffsets.push_back(0);}
    }
    ffi_cif variadicCif{};auto *callCif=&signature.cif;
    std::vector<ffi_type *> callTypes;
    if(signature.nativeVariadic){
      callTypes=signature.arguments;callTypes.insert(callTypes.end(),variadicTypes.begin(),variadicTypes.end());
      for(auto offset:variadicOffsets)pointers.push_back(variadic.data()+offset);
      if(ffi_prep_cif_var(&variadicCif,FFI_DEFAULT_ABI,signature.fixed,unsigned(callTypes.size()),ffi(t.result),callTypes.data())!=FFI_OK)
        throw std::runtime_error("Original variadic calling convention is unavailable");
      callCif=&variadicCif;
    }else if(signature.formatted||signature.wordVariadic){
      va.stack=variadic.data();auto storage=bytes(sizeof(void*));auto pointer=&va;std::memcpy(storage.data(),&pointer,sizeof pointer);pointers[signature.fixed]=storage.data();
    }
    if(interfaceQuery && words[interfaceQuery->output]){
      auto index=interfaceQuery->output;
      // Only this original interface-query contract treats its erased void*
      // as a pointer-sized output slot. Ordinary byte buffers remain direct.
      // Seed it with the original guest bits so an untouched error result
      // preserves the caller's storage exactly.
      interfaceOutput=reinterpret_cast<void *>(uintptr_t(read(memory.translate(uint32_t(words[index]),4,Memory::Read|Memory::Write),4)));
      interfaceInitial=interfaceOutput;
      auto output=&interfaceOutput;std::memcpy(pointers[index],&output,sizeof output);
      void *identifier=nullptr;std::memcpy(&identifier,pointers[interfaceQuery->identifier],sizeof identifier);
      if(identifier){
        void *canonical=nullptr;
        // Relocation/dlsym can bind data from a retained local namespace. The
        // existing binding key owns that exact provider even when a later
        // global lookup cannot see it. Reuse it before considering globals.
        {
          std::lock_guard lock(mutex);
          for(const auto &candidate:interfaces){
            if(candidate.family!=interfaceQuery->family)continue;
            for(auto it=gates.lower_bound({candidate.name,nullptr});it!=gates.end()&&it->first.first==candidate.name;++it){
              void *known=nullptr;std::memcpy(&known,it->first.second,sizeof known);
              if(!known)continue;
              if(known==identifier){interfaceType=candidate.type;canonical=known;break;}
              if(!canonical&&!std::memcmp(known,identifier,candidate.identifierBytes)){interfaceType=candidate.type;canonical=known;}
            }
            if(canonical==identifier)break;
          }
        }
        if(!canonical)for(const auto &candidate:interfaces){
          if(candidate.family!=interfaceQuery->family)continue;
          auto exported=lookup(candidate.name);if(!exported)continue;
          void *known=nullptr;std::memcpy(&known,exported,sizeof known);
          if(known&&(known==identifier||!std::memcmp(known,identifier,candidate.identifierBytes))){interfaceType=candidate.type;canonical=known;break;}
        }
        // UUID equality and the native owner's canonical identity describe
        // the same interface, including an application-owned copy of its ID.
        if(canonical)std::memcpy(pointers[interfaceQuery->identifier],&canonical,sizeof canonical);
      }
    }
    auto result=bytes(size_t(std::max(16,resultType.hostSize)));
    // Project this proc node at the existing width boundary, then retain the
    // original libc operation and its actual descriptor/stream lifetime.
    std::string procPath;
    int pathArgument=-1;
    if(name=="openat"||name=="openat64"||name=="__openat_2"||name=="faccessat"||name=="faccessat2"||name=="readlinkat"||name=="fstatat"||name=="fstatat64")pathArgument=1;
    else if(name=="open"||name=="open64"||name=="__open_2"||name=="fopen"||name=="fopen64"||name=="freopen"||name=="freopen64"||name=="stat"||name=="stat64"||name=="lstat"||name=="lstat64"||name=="access"||name=="readlink"||name=="opendir"||name=="statfs"||name=="statfs64"||name=="statvfs"||name=="statvfs64")pathArgument=0;
    if(pathArgument>=0&&unsigned(pathArgument)<signature.fixed){
      const char *path=nullptr;std::memcpy(&path,pointers[unsigned(pathArgument)],sizeof path);
      if(selfMaps(path)){
          try{procPath=mapsPath();}
          catch(const std::system_error &error){transfer.finish();errno=error.code().value();return resultType.kind==Kind::Pointer?0:UINT32_MAX;}
          path=procPath.c_str();std::memcpy(pointers[unsigned(pathArgument)],&path,sizeof path);
      }
    }
    synchronizeData(true);
    const Field *zlibState=nullptr;void *previousZlibState=nullptr,*nativeZlibRecord=nullptr;
    if(zlibRecord){
      for(unsigned i=0;i<zlibRecord->fieldCount;i++)if(std::string_view(zlibRecord->fields[i].name)=="state")zlibState=zlibRecord->fields+i;
      if(!zlibState)unsupported(*zlibRecord);
      std::memcpy(&nativeZlibRecord,pointers[0],sizeof nativeZlibRecord);
      if(nativeZlibRecord)std::memcpy(&previousZlibState,static_cast<uint8_t*>(nativeZlibRecord)+zlibState->hostOffset/8,sizeof previousZlibState);
    }
    NativeSpan previousAllocation{nullptr,0};
    if(name=="free"||reallocating){
      void *pointer=nullptr;std::memcpy(&pointer,pointers[0],sizeof pointer);
      previousAllocation={pointer,pointer?allocationExtent(pointer):size_t(1)};
      if(name=="free")retireResources(std::span(&previousAllocation,1));
    }else if(name=="freeaddrinfo"){
      // POSIX gives this owner the complete resolver list, including its
      // strings, address storage and linked nodes. Retire their representations
      // before the original function releases any of that native storage.
      void *pointer=nullptr;std::memcpy(&pointer,pointers[0],sizeof pointer);
      retireResolver(pointer,types[t.parameters[0]].element);
    }
    errno=incomingError;
    ffi_call(callCif,FFI_FN((signature.target?signature.target:native)),result.data(),pointers.data());
    struct NativeError {int value;~NativeError(){errno=value;}} nativeError{errno};
    bool finishedZlibRecord=false;
    if(nativeZlibRecord){
      void *state=nullptr;std::memcpy(&state,static_cast<uint8_t*>(nativeZlibRecord)+zlibState->hostOffset/8,sizeof state);
      finishedZlibRecord=!state;
      if(zlibEnd&&previousZlibState&&!state){const NativeSpan retired{previousZlibState,1};retireResources(std::span(&retired,1));}
    }
    if(reallocating&&(read(result.data(),sizeof(void *))||words[1]==0||(name=="reallocarray"&&words[2]==0)))
      retireResources(std::span(&previousAllocation,1));
    size_t allocationSize=0;bool allocation=false;
    if(name=="malloc" || name=="valloc" || name=="pvalloc") {allocation=true;allocationSize=size_t(words[0]);}
    else if(name=="calloc") {allocation=true;allocationSize=size_t(words[0])*size_t(words[1]);}
    else if(name=="realloc" || name=="memalign" || name=="aligned_alloc") {allocation=true;allocationSize=size_t(words[1]);}
    else if(name=="reallocarray") {allocation=true;allocationSize=size_t(words[1])*size_t(words[2]);}
    else if(name=="strdup" || name=="strndup" || name=="wcsdup") {allocation=true;void *p=nullptr;std::memcpy(&p,result.data(),sizeof p);if(p)allocationSize=name=="wcsdup"?(std::wcslen(static_cast<wchar_t *>(p))+1)*sizeof(wchar_t):std::strlen(static_cast<char *>(p))+1;}
    uint32_t publishedAllocation=0;
    if(allocation){
      void *p=nullptr;std::memcpy(&p,result.data(),sizeof p);
      // Retire the previous lifetime before refreshing any converted data.
      // Native owners may have freed a typed resource internally, and the
      // allocator can now return that same address for unrelated storage.
      publishedAllocation=publishAllocation(p,t.result,allocationSize,name);
    }else if(name=="posix_memalign" && read(result.data(),4)==0){
      void *p=nullptr;std::memcpy(&p,*reinterpret_cast<void **>(pointers[0]),sizeof p);
      publishAllocation(p,t.parameters[0],size_t(words[2]),name);
    }
    // The resource owner closes stdio storage. Retire only its direct opaque
    // mapping, leaving original static standard-stream representations alone.
    if(name=="fclose" || name=="freopen" || name=="freopen64") {
      const auto argument=name=="fclose"?0u:2u;
      void *previous=nullptr;std::memcpy(&previous,pointers[argument],sizeof previous);
      void *next=nullptr;if(name!="fclose")std::memcpy(&next,result.data(),sizeof next);
      if(previous && previous!=next)if(auto address=memory.addressOf(previous))memory.unmap(address);
    }
    // A byte operation may have changed a converted guest cell. Commit those
    // writes before refreshing representations from their native owners;
    // otherwise that refresh would silently discard the operation's result.
    transfer.commitByteWrites();synchronizeData(false);
    if(finishedZlibRecord)releaseRecord(uint32_t(words[0]));
    if(threadExitRequested && threadExitRequested()){
      transfer.finish();
      throw ManagedThreadExit{};
    }
    uint64_t answer=0;
    if(name=="dlsym" || name=="dlvsym") {
      void *resolved=nullptr;std::memcpy(&resolved,result.data(),sizeof resolved);
      if(reinterpret_cast<uintptr_t>(resolved)<=UINT32_MAX)answer=uintptr_t(resolved);
      else answer=resolveNative(memory.string(uint32_t(words[1])),resolved);
    } else if(name=="glGetString" || name=="glGetStringi") {
      // Khronos declares these strings as GLubyte*, unlike libc's char*.
      // Their original context owns the NUL-terminated bytes; expose that
      // complete extent without copying or introducing a string owner.
      void *p=nullptr;std::memcpy(&p,result.data(),sizeof p);
      answer=address(p,t.result,p?std::strlen(static_cast<const char *>(p))+1:0,name);
    } else if(allocation) {
      // The original allocator owns capacity, including malloc(0) and rounded
      // allocations. Guests can query malloc_usable_size and legitimately use
      // those bytes; a one-byte request is not the native allocation's bound.
      answer=publishedAllocation;
    } else if(name=="posix_memalign" && read(result.data(),4)==0) {
      // The output pointer is a width-converted view; register the full native
      // allocation before ordinary pointer copy-back.
      transfer.convert(t.result,result.data(),&answer,false);
    } else if(aggregateResult)transfer.convert(t.result,result.data(),memory.translate(aggregateResult,size_t(resultType.guestSize),Memory::Write),false);
    else {if(resultType.guestSize>8)unsupported(resultType);transfer.convert(t.result,result.data(),&answer,false);}
    if(interfaceQuery && words[interfaceQuery->output] && interfaceOutput!=interfaceInitial){
      auto index=interfaceQuery->output;
      // Unknown extension IDs retain ordinary opaque pointer handling. A
      // known original IID supplies its complete interface/vtable type.
      transfer.convert(interfaceType?interfaceType:t.parameters[index],&interfaceOutput,
        memory.translate(uint32_t(words[index]),4,Memory::Write),false);
    }
    transfer.finish();return aggregateResult?aggregateResult:answer;
  }
};
thread_local Imports::Impl::CallFrame *Imports::Impl::CallFrame::current=nullptr;
Imports::Imports(Memory &memory,void *(*lookup)(const char *)):impl(std::make_unique<Impl>(memory,lookup)){
  impl->libcSyscall=[this](Thread &thread)->uint64_t{
    // Original Bionic arch-arm syscall.S shifts r1-r3 and four C stack words
    // into kernel r0-r6. No type-based alignment is inserted by syscall().
    std::array<uint32_t,7> registers{};
    for(unsigned i=0;i<registers.size();i++)registers[i]=thread.argument(i+1);
    auto result=kernelCall(thread,thread.argument(0),registers);
    if(result<0&&result>=-4095){errno=impl->convertErrno(int(-result),false);return UINT32_MAX;}
    return uint32_t(result);
  };
}
Imports::~Imports()=default;
uint32_t Imports::caller()const{auto *frame=Impl::CallFrame::current;return frame && &frame->owner==impl.get()?frame->thread.returnAddress():0;}
void Imports::setThreadFactory(std::function<std::shared_ptr<Thread>()> factory){impl->threadFactory=std::move(factory);}
void Imports::setCallback(std::function<uint64_t(Thread &,uint32_t,std::span<const Value>)> callback){impl->invokeCallback=std::move(callback);}
void Imports::setJniAdapter(std::function<uint64_t(JniValue,uint64_t,bool)> adapter){impl->adaptJni=std::move(adapter);}
void Imports::setThreadExitProbe(int (*probe)(void)){impl->threadExitRequested=probe;}
void Imports::retireThread(void *native){if(auto address=impl->memory.addressOf(native))impl->memory.unmap(address);}
uint32_t Imports::resolve(std::string_view name) {
  if(auto gate=impl->resolveMemory(name))return gate;
  auto *native=impl->lookup(std::string(name).c_str());if(!native)return 0;
  return impl->resolveNative(name,native);
}
uint32_t Imports::resolve(std::string_view name,void *native){
  if(auto gate=impl->resolveMemory(name))return gate;
  return impl->resolveNative(name,native);
}
int64_t Imports::syscall(Thread &thread,uint32_t number) {
  std::array<uint32_t,7> registers{};
  for(unsigned i=0;i<registers.size();i++)registers[i]=thread.syscallArgument(i);
  return kernelCall(thread,number,registers);
}
int64_t Imports::kernelCall(Thread &thread,uint32_t number,const std::array<uint32_t,7> &registers) {
  // Raw SVC does not write libc errno. The original ARM caller performs that
  // step if it is a libc stub. Preserve the shared host cell while translating
  // the actual owner's error to the Linux negative-result convention.
  struct RestoreErrno {int previous=errno;~RestoreErrno(){errno=previous;}} restore;
  auto error=[&](int value)->int64_t{return -int64_t(impl->convertErrno(value,true));};
  auto finish=[&](uint64_t result)->int64_t{
    return uint32_t(result)==UINT32_MAX&&errno?error(errno):int64_t(uint32_t(result));
  };
  try {
    if(number==syscallFutex || number==syscallFutexTime64){
      using Call=long (*)(long,long,long,long,long,long,long);
      auto owner=reinterpret_cast<Call>(impl->lookup("syscall"));if(!owner)return -int64_t(syscallNoSys);
      const auto command=registers[1]&futex_CMD_MASK;
      const bool priority=command==futex_LOCK_PI||command==futex_LOCK_PI2||
        command==futex_UNLOCK_PI||command==futex_TRYLOCK_PI;
      void *address=impl->memory.translate(registers[0],4,Memory::Read|(priority?Memory::Write:0u));
      long timeout=registers[3];std::array<int64_t,2> time{};
      const bool timed=command==futex_WAIT||command==futex_WAIT_BITSET||
        command==futex_LOCK_PI||command==futex_LOCK_PI2||command==futex_WAIT_REQUEUE_PI;
      if(timed && registers[3]){
        if(number==syscallFutexTime64)std::memcpy(time.data(),impl->memory.translate(registers[3],16,Memory::Read),16);
        else{int32_t narrow[2];std::memcpy(narrow,impl->memory.translate(registers[3],8,Memory::Read),8);time={narrow[0],narrow[1]};}
        timeout=reinterpret_cast<long>(time.data());
      }
      void *second=nullptr;
      if(command==futex_REQUEUE||command==futex_CMP_REQUEUE||command==futex_WAKE_OP||
         command==futex_WAIT_REQUEUE_PI||command==futex_CMP_REQUEUE_PI){
        const bool writes=command==futex_WAKE_OP||command==futex_WAIT_REQUEUE_PI||command==futex_CMP_REQUEUE_PI;
        second=impl->memory.translate(registers[4],4,Memory::Read|(writes?Memory::Write:0u));
      }
      errno=0;return finish(owner(syscallFutex64,reinterpret_cast<long>(address),registers[1],registers[2],timeout,reinterpret_cast<long>(second),registers[5]));
    }
    const auto found=std::find_if(std::begin(syscalls),std::end(syscalls),[&](const auto &row){return row.number==number;});
    if(found==std::end(syscalls) || found->mode==SyscallMode::Unavailable)return -int64_t(syscallNoSys);
    const auto &row=*found;std::vector<Value> values;
    for(unsigned i=0;i<row.count;i++){
      const auto &argument=row.arguments[i];uint64_t bits=registers[argument.offset];
      if(argument.words==2)bits|=uint64_t(registers[argument.offset+1])<<32;
      values.push_back({bits,argument.words});
    }
    auto call=[&]()->uint64_t{auto gate=resolve(row.service);if(!gate){errno=ENOSYS;return UINT32_MAX;}return thread.call(gate,values);};
    errno=0;
    switch(row.mode){
    case SyscallMode::Tls: thread.setTls(registers[0]);return 0;
    case SyscallMode::CacheFlush:
      if(registers[2] || registers[1]<registers[0])return -int64_t(syscallInvalid);
      if(registers[1]!=registers[0])impl->memory.translate(registers[0],registers[1]-registers[0],Memory::Read);
      impl->memory.invalidate(registers[0],registers[1]-registers[0]);return 0;
    case SyscallMode::OpenAt: {
      auto owner=reinterpret_cast<int (*)(int,const char *,int,int)>(impl->lookup(row.service));
      if(!owner)return -int64_t(syscallNoSys);
      auto filename=impl->memory.string(registers[1]);
      if(impl->selfMaps(filename.c_str())){
        try{filename=impl->mapsPath();}
        catch(const std::system_error &failure){return error(failure.code().value());}
      }
      return finish(owner(int(registers[0]),filename.c_str(),int(registers[2]),int(registers[3])));
    }
    case SyscallMode::Ptrace: {
      auto owner=reinterpret_cast<long (*)(int,int,void *,void *)>(impl->lookup(row.service));
      if(!owner)return -int64_t(syscallNoSys);
      // Addresses are opaque tracer operands. The common process owner
      // validates target authority before any operation or dereference.
      return finish(owner(int(registers[0]),int(registers[1]),reinterpret_cast<void *>(uintptr_t(registers[2])),reinterpret_cast<void *>(uintptr_t(registers[3]))));
    }
    case SyscallMode::Seek: {
      auto *output=impl->memory.translate(registers[3],8,Memory::Write);
      values={Value::word(registers[0]),Value::wide((uint64_t(registers[1])<<32)|registers[2]),Value::word(registers[4])};
      auto result=call();if(result==UINT64_MAX&&errno)return error(errno);
      std::memcpy(output,&result,8);return 0;
    }
    case SyscallMode::Getcwd:
      if(!registers[1])return -int64_t(syscallRange);
      impl->memory.translate(registers[0],registers[1],Memory::Write);break;
    case SyscallMode::AppendZero:values.push_back(Value::word(0));break;
    case SyscallMode::Statfs: {
      auto f=std::find_if(std::begin(functions),std::end(functions),[&](const auto &entry){return std::string_view(entry.name)==row.service;});
      if(f==std::end(functions))return -int64_t(syscallNoSys);
      auto pointer=types[f->type].parameters[1];
      if(values[1].bits!=unsigned(types[types[pointer].element].guestSize))return -int64_t(syscallInvalid);
      values.erase(values.begin()+1);break;
    }
    case SyscallMode::VectorOffset: {
      auto offset=Value::wide(values[3].bits|(values[4].bits<<32));
      values.erase(values.begin()+3,values.begin()+5);values.insert(values.begin()+3,offset);break;
    }
    case SyscallMode::Advice:values={values[0],values[2],values[3],values[1]};break;
    case SyscallMode::SyncRange:values={values[0],values[2],values[3],values[1]};break;
    case SyscallMode::SignalSize:
      if(values.back().bits!=8)return -int64_t(syscallInvalid);
      values.pop_back();break;
    case SyscallMode::SignalFd:
      if(values[2].bits!=8)return -int64_t(syscallInvalid);
      values.erase(values.begin()+2);break;
    default:break;
    }
    auto result=call();
    if(uint32_t(result)==UINT32_MAX&&errno)return error(errno);
    if(row.mode==SyscallMode::Getcwd){if(!result)return error(errno);return int64_t(impl->memory.string(uint32_t(result)).size()+1);}
    if(row.mode==SyscallMode::ErrorNumber || row.mode==SyscallMode::Advice)return -int64_t(uint32_t(result));
    if(row.mode==SyscallMode::Priority){if(uint32_t(result)==UINT32_MAX&&errno)return error(errno);return 20-int64_t(int32_t(result));}
    if(row.voidResult)return 0;
    return finish(result);
  }catch(const Memory::Fault &){return -int64_t(syscallFault);}
}
}
