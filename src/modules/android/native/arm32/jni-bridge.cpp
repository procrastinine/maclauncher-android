#include "jni-bridge.h"
#include "managed-exit.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if __has_include(<ffi/ffi.h>)
#include <ffi/ffi.h>
#else
#include <ffi.h>
#endif
#if !defined(__ANDROID__)
#include <jvmti.h>
#include <mach-o/dyld.h>
#else
#include <pthread.h>
#endif
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace maclauncher::arm32 {
namespace {
template <class T> T load(const void *p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
template <class T> void store(void *p, T v) { std::memcpy(p, &v, sizeof v); }
std::vector<char> parameterTypes(std::string_view signature) {
  if (signature.empty() || signature.front() != '(')
    throw std::runtime_error("Invalid JNI method descriptor");
  std::vector<char> types;
  size_t i = 1;
  while (i < signature.size() && signature[i] != ')') {
    char type = signature[i++];
    bool array = false;
    while (type == '[') {
      array = true;
      if (i == signature.size())
        throw std::runtime_error("Truncated JNI array descriptor");
      type = signature[i++];
    }
    if (type == 'L') {
      auto end = signature.find(';', i);
      if (end == std::string_view::npos)
        throw std::runtime_error("Truncated JNI object descriptor");
      i = end + 1;
    } else if (std::string_view("ZBCSIJFD").find(type) ==
               std::string_view::npos)
      throw std::runtime_error("Invalid JNI parameter descriptor");
    types.push_back(array ? 'L' : type);
  }
  if (i + 1 >= signature.size() || signature[i] != ')')
    throw std::runtime_error("Truncated JNI return descriptor");
  return types;
}
char returnType(std::string_view signature) {
  auto p = signature.find(')');
  if (p == std::string_view::npos || p + 1 == signature.size())
    throw std::runtime_error("Invalid JNI return descriptor");
  return signature[p + 1] == '[' ? 'L' : signature[p + 1];
}
std::string shortyDescriptor(std::string_view shorty) {
  if(shorty.empty())throw std::runtime_error("JNI method shorty is unavailable");
  std::string value;
  auto append=[&](char type,bool result){
    if(type=='L')value+="Ljava/lang/Object;";
    else if(std::string_view("ZBCSIJFD").find(type)!=std::string_view::npos || (result && type=='V'))value+=type;
    else throw std::runtime_error("Invalid JNI method shorty");
  };
  value+='(';for(char type:shorty.substr(1))append(type,false);
  value+=')';append(shorty.front(),true);return value;
}
ffi_type *ffiType(char type) {
  switch (type) {
  case 'V':
    return &ffi_type_void;
  case 'Z':
    return &ffi_type_uint8;
  case 'B':
    return &ffi_type_sint8;
  case 'C':
    return &ffi_type_uint16;
  case 'S':
    return &ffi_type_sint16;
  case 'I':
    return &ffi_type_sint32;
  case 'J':
    return &ffi_type_sint64;
  case 'F':
    return &ffi_type_float;
  case 'D':
    return &ffi_type_double;
  case 'L':
    return &ffi_type_pointer;
  }
  throw std::runtime_error("Invalid JNI native descriptor");
}
#define JNI_RESULTS(X)                                                         \
  X(Object, jobject, l)                                                        \
  X(Boolean, jboolean, z)                                                      \
  X(Byte, jbyte, b) X(Char, jchar, c) X(Short, jshort, s) X(Int, jint, i)      \
      X(Long, jlong, j) X(Float, jfloat, f) X(Double, jdouble, d)
enum class MethodKind {
  NewObject,
  Void,
  StaticVoid,
  NonvirtualVoid,
#define METHOD_ENUM(Name, Type, Field) Name, Static##Name, Nonvirtual##Name,
  JNI_RESULTS(METHOD_ENUM)
#undef METHOD_ENUM
};
enum class ArgumentKind { Registers, VaList, Array };
enum class Reference { Local, Global, Weak, Opaque };
} // namespace

struct Jni::Impl {
  struct Invocation {
    const KljJniInvocationApi *api;
    KljJniInvocation scope{};
    bool entered;
    Invocation(const KljJniInvocationApi *api,JNIEnv *env)
        : api(api),entered(!api || api->enter(&scope,env)) {
      if(api && entered)api->deferExit(&scope);
    }
    ~Invocation(){if(api && entered)api->leave(&scope);}
  };
  struct Handle {
    void *value;
    Reference kind;
    uint64_t frame;
  };
  struct Context {
    Impl &owner;
    Thread &thread;
    JNIEnv *environment;
    Context *previous;
    std::vector<uint64_t> frames;
    Context(Impl &owner, Thread &thread, JNIEnv *environment)
        : owner(owner), thread(thread), environment(environment),
          previous(current) {
      attach(environment);
      current = this;
    }
    void attach(JNIEnv *value) {
      environment=value;
      if(value && frames.empty()) {
        if(value->PushLocalFrame(16)!=JNI_OK)throw std::bad_alloc();
        frames.push_back(owner.newFrame());
      }
    }
    jobject finish(jobject result = nullptr) {
      while (frames.size() > 1) {
        environment->PopLocalFrame(nullptr);
        owner.retireFrame(frames.back());
        frames.pop_back();
      }
      if (frames.empty())
        return result;
      result = environment->PopLocalFrame(result);
      owner.retireFrame(frames.back());
      frames.clear();
      return result;
    }
    ~Context() {
      finish();
      current = previous;
    }
  };
  static thread_local Context *current;
  Memory &memory;
  JavaVM *vm;
  void (*nativeThreadEntry)();
  const KljJniInvocationApi *invocation;
  Metadata metadata;
#if !defined(__ANDROID__)
  jvmtiEnv *ti = nullptr;
#endif
  std::mutex mutex;
  uint32_t nextHandle = 1;
  uint64_t nextFrame = 1;
  std::unordered_map<uint32_t, Handle> handles;
  std::vector<uint32_t> freeHandles;
  std::unordered_map<uint64_t, std::vector<uint32_t>> frameHandles;
  std::unordered_map<void *, uint32_t> opaqueHandles;
  std::unordered_map<jmethodID, std::string> signatures;
  std::vector<uint32_t> gates;
  uint32_t environment = 0, virtualVm = 0;
  struct Buffer {
    void *host;
    size_t size;
    jobject owner;
    std::function<void(JNIEnv *)> cleanup;
    bool weak = false;
  };
  std::unordered_map<uint32_t, Buffer> buffers;
  struct ThreadPool {
    Memory &memory;
    std::mutex mutex;
    std::map<std::thread::id, std::shared_ptr<Thread>> threads;
    std::function<int64_t(Thread &,uint32_t)> syscall;
    explicit ThreadPool(Memory &memory) : memory(memory) {}
  };
  struct ThreadLease {
    std::weak_ptr<ThreadPool> pool;
    std::thread::id id;
    ThreadLease(std::shared_ptr<ThreadPool> pool, std::thread::id id)
        : pool(pool), id(id) {}
    ~ThreadLease() {
      if (auto owner = pool.lock()) {
        std::lock_guard lock(owner->mutex);
        owner->threads.erase(id);
      }
    }
  };
  std::shared_ptr<ThreadPool> threadPool;
  struct ThreadOwners {
    const KljJniInvocationApi *physicalOwner=nullptr;
    std::vector<std::unique_ptr<ThreadLease>> leases;
    static void retire(void *opaque){
      auto *owner=static_cast<ThreadOwners *>(opaque);
      // Guest C++/POSIX callbacks can reenter any of this thread's JNI pools.
      // Finish that physical lifetime while all pool leases and their vector
      // still live. A later native TLS callback must see an empty control
      // identity, never a C++ thread_local object whose destructor already ran.
      if(owner->physicalOwner)owner->physicalOwner->physicalEnd();
      threadOwners=nullptr;
      delete owner;
    }
  };
  static thread_local ThreadOwners *threadOwners;
  static ThreadOwners &retainThreadOwners(){
    if(!threadOwners){
      auto owner=std::make_unique<ThreadOwners>();
#if defined(__ANDROID__)
      // Original Bionic invokes this destructor on the guest thread and stack.
      static const pthread_key_t key=[] {
        pthread_key_t value;
        if(pthread_key_create(&value,ThreadOwners::retire))
          throw std::runtime_error("Cannot register JNI thread retirement");
        return value;
      }();
      if(pthread_setspecific(key,owner.get()))
        throw std::runtime_error("Cannot retain JNI thread lifetime");
#else
      _tlv_atexit(ThreadOwners::retire,owner.get());
#endif
      threadOwners=owner.release();
    }
    return *threadOwners;
  }
  std::shared_ptr<Thread> guestThread() {
    auto id = std::this_thread::get_id();
    std::lock_guard lock(threadPool->mutex);
    auto found = threadPool->threads.find(id);
    if (found != threadPool->threads.end())
      return found->second;
    auto &owners=retainThreadOwners();
    auto thread = std::make_shared<Thread>(memory);
    thread->setSyscall(threadPool->syscall);
    if(invocation)owners.physicalOwner=invocation;
    std::erase_if(owners.leases,
                  [](auto &lease) { return lease->pool.expired(); });
    owners.leases.push_back(std::make_unique<ThreadLease>(threadPool, id));
    threadPool->threads.emplace(id, thread);
    return thread;
  }
  struct Native : std::enable_shared_from_this<Native> {
    Impl &owner;
    std::string name, signature;
    uint32_t function;
    NativeCall callType;
    std::vector<char> parameters;
    std::vector<ffi_type *> types;
    ffi_cif cif{};
    ffi_closure *closure = nullptr;
    void *code = nullptr;
    Native(Impl &owner, std::string name, std::string signature,
           uint32_t function, NativeCall callType = NativeCall::Regular)
        : owner(owner), name(std::move(name)), signature(std::move(signature)),
          function(function), callType(callType), parameters(parameterTypes(this->signature)) {
      if(callType!=NativeCall::Critical)types = {&ffi_type_pointer, &ffi_type_pointer};
      else if(returnType(this->signature)=='L' || std::find(parameters.begin(),parameters.end(),'L')!=parameters.end())
        throw std::runtime_error("Critical native methods cannot use JNI references");
      for (auto type : parameters)
        types.push_back(ffiType(type));
      if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, unsigned(types.size()),
                       ffiType(returnType(this->signature)),
                       types.data()) != FFI_OK)
        throw std::runtime_error(
            "Cannot prepare JNI native calling convention");
      closure = static_cast<ffi_closure *>(
          ffi_closure_alloc(sizeof(ffi_closure), &code));
      if (!closure)
        throw std::bad_alloc();
      if (ffi_prep_closure_loc(closure, &cif, callback, this, code) != FFI_OK) {
        ffi_closure_free(closure);
        closure = nullptr;
        throw std::runtime_error("Cannot prepare JNI native callback");
      }
    }
    ~Native() {
      if (closure)
        ffi_closure_free(closure);
    }
    static void callback(ffi_cif *cif, void *result, void **args,
                         void *user) noexcept {
      auto retained = static_cast<Native *>(user)->shared_from_this();
      auto &native = *retained;
      const bool critical=native.callType==NativeCall::Critical;
      const bool vmCall=native.callType==NativeCall::Invocation;
      JNIEnv *env=nullptr;
      if(vmCall)native.owner.vm->GetEnv(reinterpret_cast<void **>(&env),JNI_VERSION_1_6);
      else if(!critical)env=load<JNIEnv *>(args[0]);
      try {
        if(vmCall&&(!env||load<JavaVM *>(args[0])!=native.owner.vm))throw std::runtime_error("JNI invocation belongs to another VM");
        Invocation invocation(critical?nullptr:native.owner.invocation,env);
        if(!invocation.entered) {
          if(cif->rtype!=&ffi_type_void)std::memset(result,0,cif->rtype->size);
          return;
        }
        std::shared_ptr<Thread> local;
        Thread *thread = current && &current->owner == &native.owner
                             ? &current->thread
                             : nullptr;
        if (!thread) {
          local = native.owner.guestThread();
          thread = local.get();
        }
        std::vector<jvalue> values(native.parameters.size());
        for (size_t i = 0; i < values.size(); i++)
          std::memcpy(&values[i], args[i + (critical?0:2)],
                      ffiType(native.parameters[i])->size);
        jvalue value{};
        if(vmCall)value.j=native.owner.invokeVm(*thread,env,native.function,load<void *>(args[1]));
        else value=native.owner.call(*thread, env, native.function,
                              critical?nullptr:load<jobject>(args[1]), native.signature, values,critical);
        if (cif->rtype != &ffi_type_void)
          std::memcpy(result, &value, cif->rtype->size);
      } catch (const ManagedThreadExit &) {
        if(critical)std::abort(); // Critical natives have no JNI exception channel.
        // The shared owner leaves the original ThreadDeath pending; returning
        // normally lets the JVM retire its own thread and Java frames.
        if(cif->rtype!=&ffi_type_void)std::memset(result,0,cif->rtype->size);
      } catch (const std::exception &error) {
        if(critical){std::fprintf(stderr,"Critical native execution failed: %s\n",error.what());std::abort();}
        if (cif->rtype != &ffi_type_void)
          std::memset(result, 0, cif->rtype->size);
        if (env&&!env->ExceptionCheck()) {
          auto type = env->FindClass("java/lang/UnsatisfiedLinkError");
          if (type)
            env->ThrowNew(type, error.what());
        }
      }
    }
  };
  struct Registration {
    jclass type;
    std::vector<std::shared_ptr<Native>> methods;
  };
  std::vector<Registration> registrations;
  std::map<std::tuple<uint32_t,std::string,NativeCall>,std::shared_ptr<Native>> trampolines;

  explicit Impl(Memory &addressSpace, JavaVM *machine,void (*threadEntry)(),
                const KljJniInvocationApi *invocationApi, Metadata methodMetadata)
      : memory(addressSpace), vm(machine), nativeThreadEntry(threadEntry),
        invocation(invocationApi), metadata(methodMetadata),
        threadPool(std::make_shared<ThreadPool>(addressSpace)) {
    if(!metadata.methodShorty){
#if !defined(__ANDROID__)
      if (vm->GetEnv(reinterpret_cast<void **>(&ti), JVMTI_VERSION_1_2) != JNI_OK)
        throw std::runtime_error("JVM method metadata interface is unavailable");
#else
      throw std::runtime_error("ART method metadata interface is unavailable");
#endif
    }
    std::array<uint32_t, sizeof(*std::declval<JNIEnv>().functions) / sizeof(void *)> slots{};
#include "jni-table.inc"
    environment = memory.allocate(0x60000000, uint32_t((slots.size() + 1) * 4),
                                  Memory::Read|Memory::Write);
    store(memory.loadAddress(environment, 4), environment + 4);
    std::memcpy(memory.loadAddress(environment + 4, slots.size() * 4),
                slots.data(), slots.size() * 4);
    std::array<uint32_t, sizeof(*std::declval<JavaVM>().functions) / sizeof(void *)>
        invocation{};
#include "jni-invocation.inc"
    virtualVm = memory.allocate(
        0x60000000, uint32_t((invocation.size() + 1) * 4), Memory::Read|Memory::Write);
    store(memory.loadAddress(virtualVm, 4), virtualVm + 4);
    std::memcpy(memory.loadAddress(virtualVm + 4, invocation.size() * 4),
                invocation.data(), invocation.size() * 4);
  }
  ~Impl() {
    JNIEnv *e = nullptr;
    bool attached = vm->GetEnv(reinterpret_cast<void **>(&e),
                               JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached)
      attachVm(&e, nullptr, true);
    if (e) {
      for (auto &[address, buffer] : buffers) {
        (void)address;
        if (buffer.cleanup)
          buffer.cleanup(e);
        if (buffer.owner) {
          if (buffer.weak)
            e->DeleteWeakGlobalRef(buffer.owner);
          else
            e->DeleteGlobalRef(buffer.owner);
        }
      }
      for (auto &registration : registrations) {
        e->UnregisterNatives(registration.type);
        e->DeleteGlobalRef(registration.type);
      }
      for (auto &[token, handle] : handles) {
        (void)token;
        if (handle.kind == Reference::Global)
          e->DeleteGlobalRef(static_cast<jobject>(handle.value));
        else if (handle.kind == Reference::Weak)
          e->DeleteWeakGlobalRef(static_cast<jweak>(handle.value));
      }
    }
    // A VM's Thread.join can return before the native TLS destructors finish.
    // A retiring ThreadLease may still hold the pool, so retire its Threads
    // under the same mutex while their borrowed Memory owner is still alive.
    {
      std::lock_guard lock(threadPool->mutex);
      threadPool->threads.clear();
    }
    threadPool.reset();
    for (auto address : gates)
      memory.retireImport(address);
    for (auto &[address, buffer] : buffers) {
      (void)buffer;
      memory.unmap(address);
    }
    memory.unmap(environment);
    memory.unmap(virtualVm);
    if (attached && e)
      vm->DetachCurrentThread();
  }
  uint64_t newFrame() {
    std::lock_guard lock(mutex);
    return nextFrame++;
  }
  jint attachVm(JNIEnv **environment, void *arguments, bool daemon) {
#if defined(__ANDROID__)
    auto output=environment;
#else
    auto output=reinterpret_cast<void **>(environment);
#endif
    return daemon ? vm->AttachCurrentThreadAsDaemon(output,arguments)
                  : vm->AttachCurrentThread(output,arguments);
  }
  void retireFrame(uint64_t frame) {
    std::lock_guard lock(mutex);
    auto group = frameHandles.find(frame);
    if (group == frameHandles.end())
      return;
    for (auto token : group->second) {
      auto found = handles.find(token);
      if (found == handles.end() || found->second.kind != Reference::Local ||
          found->second.frame != frame)
        continue;
      handles.erase(found);
      freeHandles.push_back(token);
    }
    frameHandles.erase(group);
  }
  JNIEnv *env() {
    if (current && &current->owner == this && current->environment)
      return current->environment;
    JNIEnv *e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_6) != JNI_OK)
      throw std::runtime_error("JNI call from a detached ARM32 thread");
    if(current && &current->owner==this)current->attach(e);
    return e;
  }
  uint32_t gate(Memory::HostCall call) {
    auto address = memory.import(std::move(call));
    gates.push_back(address);
    return address;
  }
  uint32_t handle(void *value, Reference kind = Reference::Local) {
    if (!value)
      return 0;
    std::lock_guard lock(mutex);
    if (kind == Reference::Opaque) {
      auto found = opaqueHandles.find(value);
      if (found != opaqueHandles.end())
        return found->second;
    }
    uint32_t token;
    if (freeHandles.empty()) {
      if (!nextHandle)
        throw std::runtime_error("JNI handle address space exhausted");
      token = nextHandle++;
    } else {
      token = freeHandles.back();
      freeHandles.pop_back();
    }
    uint64_t frame = 0;
    if (kind == Reference::Local) {
      if (!current || &current->owner != this)
        throw std::runtime_error("JNI local reference outside an invocation");
      frame = current->frames.back();
    }
    handles.emplace(token, Handle{value, kind, frame});
    if (kind == Reference::Local)
      frameHandles[frame].push_back(token);
    if (kind == Reference::Opaque)
      opaqueHandles.emplace(value, token);
    return token;
  }
  template <class T> T reference(uint32_t token) {
    if (!token)
      return nullptr;
    std::lock_guard lock(mutex);
    auto found = handles.find(token);
    if (found == handles.end())
      throw std::runtime_error("Retired JNI reference");
    return static_cast<T>(found->second.value);
  }
  void retire(uint32_t token) {
    if (!token)
      return;
    std::lock_guard lock(mutex);
    if (handles.erase(token))
      freeHandles.push_back(token);
  }
  template <class T> uint64_t returned(T value) {
    if constexpr (std::is_convertible_v<T, jobject>)
      return handle(value);
    else if constexpr (std::is_same_v<T, jmethodID> ||
                       std::is_same_v<T, jfieldID>)
      return handle(value, Reference::Opaque);
    else if constexpr (std::is_floating_point_v<T>) {
      uint64_t bits = 0;
      std::memcpy(&bits, &value, sizeof value);
      return bits;
    } else
      return uint64_t(value);
  }
  template <class T>
  void checkBuffer(T *value, int64_t elements, unsigned permission) {
    if (elements < 0)
      return; // Original JVM reports negative array/string lengths.
    if (uint64_t(elements) > UINT32_MAX / sizeof(T))
      throw std::runtime_error("JNI buffer is too large for ARM32");
    if (!value) {
      if (elements)
        throw std::runtime_error("Null JNI buffer");
      return;
    }
    auto address = memory.addressOf(value);
    if (!address)
      throw std::runtime_error("JNI buffer is outside guest memory");
    memory.translate(address, size_t(elements) * sizeof(T), permission);
  }
  struct Cursor {
    Impl &owner;
    Thread &thread;
    unsigned index = 0;
    uint32_t address = 0;
    bool fromMemory = false;
    Cursor(Impl &owner, Thread &thread) : owner(owner), thread(thread) {}
    uint32_t word() {
      if (!fromMemory)
        return thread.argument(index++);
      auto result =
          load<uint32_t>(owner.memory.translate(address, 4, Memory::Read));
      address += 4;
      return result;
    }
    uint64_t wide() {
      if (fromMemory)
        address = (address + 7) & ~7u;
      else
        index = (index + 1) & ~1u;
      auto low = word();
      return uint64_t(low) | (uint64_t(word()) << 32);
    }
    template <class T> T take() {
      if constexpr (std::is_convertible_v<T, jobject> ||
                    std::is_same_v<T, jmethodID> || std::is_same_v<T, jfieldID>)
        return owner.reference<T>(word());
      else if constexpr (std::is_pointer_v<T>) {
        auto address = word();
        if (!address)
          return nullptr;
        using Element = std::remove_pointer_t<T>;
        if constexpr (std::is_same_v<T, const char *>)
          owner.memory.string(address);
        return reinterpret_cast<T>(owner.memory.translate(
            address,
            sizeof(std::conditional_t<std::is_void_v<Element>, char, Element>),
            std::is_const_v<Element> ? Memory::Read : Memory::Write));
      } else {
        auto bits = sizeof(T) == 8 ? wide() : word();
        T value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
      }
    }
  };
  std::string signature(jmethodID method) {
    {
      std::lock_guard lock(mutex);
      auto found = signatures.find(method);
      if (found != signatures.end())
        return found->second;
    }
    std::string value;
    if(metadata.methodShorty){
      const char *shorty=metadata.methodShorty(env(),method);
      // Only argument widths are consumed here. ART's shorty intentionally
      // collapses every reference/array to L; original JNI still owns types.
      value=shortyDescriptor(shorty?shorty:"");
    }else{
#if !defined(__ANDROID__)
      char *descriptor = nullptr;
      auto error = ti->GetMethodName(method, nullptr, &descriptor, nullptr);
      if (error != JVMTI_ERROR_NONE || !descriptor)
        throw std::runtime_error("JVM method descriptor is unavailable");
      value=descriptor;
      ti->Deallocate(reinterpret_cast<unsigned char *>(descriptor));
#endif
    }
    std::lock_guard lock(mutex);
    signatures.emplace(method, value);
    return value;
  }
  std::vector<jvalue> arguments(Cursor &c, jmethodID method,
                                ArgumentKind kind) {
    auto types = parameterTypes(signature(method));
    std::vector<jvalue> values(types.size());
    uint32_t array = 0;
    if (kind == ArgumentKind::Array)
      array = c.word();
    else if (kind == ArgumentKind::VaList) {
      c.address = c.word();
      c.fromMemory = true;
    }
    if (kind == ArgumentKind::Array && !types.empty())
      memory.translate(array, types.size() * 8, Memory::Read);
    for (size_t i = 0; i < types.size(); i++) {
      uint64_t bits =
          kind == ArgumentKind::Array
              ? load<uint64_t>(
                    memory.translate(array + uint32_t(i * 8), 8, Memory::Read))
              : (types[i] == 'J' || types[i] == 'D' || types[i] == 'F'
                     ? c.wide()
                     : c.word());
      if (types[i] == 'L')
        values[i].l = reference<jobject>(uint32_t(bits));
      else if (types[i] == 'F' && kind != ArgumentKind::Array)
        values[i].f = jfloat(load<jdouble>(&bits));
      else
        std::memcpy(&values[i], &bits, sizeof bits);
    }
    return values;
  }
  uint64_t method(Thread &t, MethodKind kind, ArgumentKind argumentKind) {
    Cursor c(*this, t);
    c.word();
    auto object = c.take<jobject>();
    jclass type = nullptr;
    bool nonvirtual = false;
    switch (kind) {
#define NONVIRTUAL(Name, Type, Field) case MethodKind::Nonvirtual##Name:
      JNI_RESULTS(NONVIRTUAL)
#undef NONVIRTUAL
    case MethodKind::NonvirtualVoid:
      nonvirtual = true;
      break;
    default:
      break;
    }
    if (nonvirtual)
      type = c.take<jclass>();
    auto id = c.take<jmethodID>();
    auto values = arguments(c, id, argumentKind);
    auto e = env();
    switch (kind) {
    case MethodKind::NewObject:
      return returned(
          e->NewObjectA(static_cast<jclass>(object), id, values.data()));
#define METHOD_CALL(Name, Type, Field)                                         \
  case MethodKind::Name:                                                       \
    return returned(e->Call##Name##MethodA(object, id, values.data()));        \
  case MethodKind::Static##Name:                                               \
    return returned(e->CallStatic##Name##MethodA(static_cast<jclass>(object),  \
                                                 id, values.data()));          \
  case MethodKind::Nonvirtual##Name:                                           \
    return returned(                                                           \
        e->CallNonvirtual##Name##MethodA(object, type, id, values.data()));
      JNI_RESULTS(METHOD_CALL)
#undef METHOD_CALL
    case MethodKind::Void:
      e->CallVoidMethodA(object, id, values.data());
      return 0;
    case MethodKind::StaticVoid:
      e->CallStaticVoidMethodA(static_cast<jclass>(object), id, values.data());
      return 0;
    case MethodKind::NonvirtualVoid:
      e->CallNonvirtualVoidMethodA(object, type, id, values.data());
      return 0;
    }
  }
  uint64_t DestroyJavaVM(Thread &) { return uint64_t(vm->DestroyJavaVM()); }
  uint64_t AttachCurrentThread(Thread &t) { return attach(t, false); }
  uint64_t DetachCurrentThread(Thread &) {
    if(invocation&&invocation->retiring())return uint64_t(JNI_OK);
    auto *context=current && &current->owner==this?current:nullptr;
    auto *previous=context?context->environment:nullptr;
    if(context)context->finish();
    auto result=invocation?invocation->detach(vm):vm->DetachCurrentThread();
    if(context)context->attach(result==JNI_OK?nullptr:previous);
    return uint64_t(result);
  }
  uint64_t AttachCurrentThreadAsDaemon(Thread &t) { return attach(t, true); }
  uint64_t GetEnv(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto output = c.word();
    auto version = c.take<jint>();
    void *host = nullptr;
    auto result = vm->GetEnv(&host, version);
    store(memory.translate(output, 4, Memory::Write),
          result == JNI_OK ? environment : 0u);
    return uint64_t(result);
  }
  uint64_t attach(Thread &t, bool daemon) {
    if(nativeThreadEntry)nativeThreadEntry();
    Cursor c(*this, t);
    c.word();
    auto output = c.word(), input = c.word();
    JavaVMAttachArgs args{};
    JNIEnv *host = nullptr;
    if (input) {
      auto raw = static_cast<const uint32_t *>(
          memory.translate(input, 12, Memory::Read));
      args.version = jint(raw[0]);
      args.name =
          raw[1] ? static_cast<char *>(memory.translate(
                       raw[1], memory.string(raw[1]).size() + 1, Memory::Read))
                 : nullptr;
      args.group = reference<jobject>(raw[2]);
    }
    auto result = attachVm(&host,input ? &args : nullptr,daemon);
    if(result==JNI_OK && current && &current->owner==this)current->attach(host);
    store(memory.translate(output, 4, Memory::Write),
          result == JNI_OK ? environment : 0u);
    return uint64_t(result);
  }
  uint64_t GetJavaVM(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto output = c.word();
    JavaVM *actual = nullptr;
    auto result = env()->GetJavaVM(&actual);
    store(memory.translate(output, 4, Memory::Write),
          result == JNI_OK ? virtualVm : 0u);
    return uint64_t(result);
  }
  uint64_t PushLocalFrame(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto result = env()->PushLocalFrame(c.take<jint>());
    if (result == JNI_OK)
      current->frames.push_back(newFrame());
    return uint64_t(result);
  }
  uint64_t PopLocalFrame(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto result = env()->PopLocalFrame(c.take<jobject>());
    auto frame = current->frames.back();
    retireFrame(frame);
    current->frames.pop_back();
    if (current->frames.empty())
      current->frames.push_back(newFrame());
    return returned(result);
  }
#define REF_OPERATION(Name, Kind)                                              \
  uint64_t New##Name##Ref(Thread &t) {                                         \
    Cursor c(*this, t);                                                        \
    c.word();                                                                  \
    return handle(env()->New##Name##Ref(c.take<jobject>()), Reference::Kind);  \
  }                                                                            \
  uint64_t Delete##Name##Ref(Thread &t) {                                      \
    Cursor c(*this, t);                                                        \
    c.word();                                                                  \
    auto token = c.word();                                                     \
    env()->Delete##Name##Ref(reference<jobject>(token));                       \
    retire(token);                                                             \
    return 0;                                                                  \
  }
  REF_OPERATION(Global, Global)
  REF_OPERATION(WeakGlobal, Weak)
#undef REF_OPERATION
  uint64_t DeleteLocalRef(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto token = c.word();
    env()->DeleteLocalRef(reference<jobject>(token));
    retire(token);
    return 0;
  }
  uint32_t borrow(const void *host, size_t bytes, jobject retained = nullptr,
                  std::function<void(JNIEnv *)> cleanup = {},
                  bool weak = false) {
    if (!host) {
      if (retained) {
        if (weak)
          env()->DeleteWeakGlobalRef(retained);
        else
          env()->DeleteGlobalRef(retained);
      }
      return 0;
    }
    if (bytes > UINT32_MAX) {
      if (cleanup)
        cleanup(env());
      if (retained) {
        if (weak)
          env()->DeleteWeakGlobalRef(retained);
        else
          env()->DeleteGlobalRef(retained);
      }
      throw std::runtime_error("JNI buffer exceeds ARM32 address space");
    }
    uint32_t address = 0;
    try {
      address = memory.borrow(const_cast<void *>(host), uint32_t(bytes),
                              Memory::Read | Memory::Write);
      std::lock_guard lock(mutex);
      buffers.emplace(address, Buffer{const_cast<void *>(host), bytes, retained,
                                      cleanup, weak});
      return address;
    } catch (...) {
      if (address)
        memory.unmap(address);
      if (cleanup)
        cleanup(env());
      if (retained) {
        if (weak)
          env()->DeleteWeakGlobalRef(retained);
        else
          env()->DeleteGlobalRef(retained);
      }
      throw;
    }
  }
  uint64_t GetStringUTFRegion(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto string = c.take<jstring>();
    auto start = c.take<jsize>(), length = c.take<jsize>();
    auto address = c.word();
    // JNI writes modified UTF-8, whose byte length depends on the actual
    // characters. Requiring three writable bytes per character would reject
    // valid smaller ASCII buffers. Preserve exactly the original JVM output,
    // including a terminator if this JVM writes one.
    std::vector<unsigned char> bytes(length < 0 ? 1 : size_t(length) * 3 + 1,
                                     0xff);
    env()->GetStringUTFRegion(string, start, length,
                             reinterpret_cast<char *>(bytes.data()));
    if (env()->ExceptionCheck())
      return 0;
    size_t count = bytes.size();
    while (count && bytes[count - 1] == 0xff)
      --count;
    if (count)
      std::memcpy(memory.translate(address, count, Memory::Write), bytes.data(),
                  count);
    return 0;
  }
  void release(uint32_t address) {
    if (!address)
      return;
    jobject retained = nullptr;
    {
      std::lock_guard lock(mutex);
      auto found = buffers.find(address);
      if (found != buffers.end()) {
        retained = found->second.owner;
        buffers.erase(found);
      }
    }
    memory.unmap(address);
    if (retained)
      env()->DeleteGlobalRef(retained);
  }
#define STRING_BUFFER(Name, Type, Length)                                      \
  uint64_t GetString##Name(Thread &t) {                                        \
    Cursor c(*this, t);                                                        \
    c.word();                                                                  \
    auto string = c.take<jstring>();                                           \
    auto copy = c.take<jboolean *>();                                          \
    auto e = env();                                                            \
    auto count = Length;                                                       \
    auto retained = static_cast<jstring>(e->NewGlobalRef(string));             \
    auto pointer = e->GetString##Name(string, copy);                           \
    return borrow(pointer, size_t(count) * sizeof(Type), retained,             \
                  [retained, pointer](JNIEnv *env) {                           \
                    env->ReleaseString##Name(retained, pointer);               \
                  });                                                          \
  }                                                                            \
  uint64_t ReleaseString##Name(Thread &t) {                                    \
    Cursor c(*this, t);                                                        \
    c.word();                                                                  \
    auto string = c.take<jstring>();                                           \
    auto address = c.word();                                                   \
    env()->ReleaseString##Name(                                                \
        string, address ? static_cast<const Type *>(                           \
                              memory.translate(address, 1, Memory::Read))      \
                        : nullptr);                                            \
    release(address);                                                          \
    return 0;                                                                  \
  }
  STRING_BUFFER(Chars, jchar, e->GetStringLength(string))
  STRING_BUFFER(UTFChars, char, e->GetStringUTFLength(string) + 1)
  STRING_BUFFER(Critical, jchar, e->GetStringLength(string))
#undef STRING_BUFFER
#define ARRAY_BUFFER(Name, Type, Field)                                        \
  uint64_t Get##Name##ArrayElements(Thread &t) {                               \
    Cursor c(*this, t);                                                        \
    c.word();                                                                  \
    auto array = c.take<Type##Array>();                                        \
    auto copy = c.take<jboolean *>();                                          \
    auto e = env();                                                            \
    auto count = e->GetArrayLength(array);                                     \
    auto retained = static_cast<Type##Array>(e->NewGlobalRef(array));          \
    auto pointer = e->Get##Name##ArrayElements(array, copy);                   \
    return borrow(pointer, size_t(count) * sizeof(Type), retained,             \
                  [retained, pointer](JNIEnv *env) {                           \
                    env->Release##Name##ArrayElements(retained, pointer, 0);   \
                  });                                                          \
  }                                                                            \
  uint64_t Release##Name##ArrayElements(Thread &t) {                           \
    Cursor c(*this, t);                                                        \
    c.word();                                                                  \
    auto array = c.take<Type##Array>();                                        \
    auto address = c.word();                                                   \
    auto mode = c.take<jint>();                                                \
    env()->Release##Name##ArrayElements(                                       \
        array,                                                                 \
        address                                                                \
            ? static_cast<Type *>(memory.translate(address, 1, Memory::Read))  \
            : nullptr,                                                         \
        mode);                                                                 \
    if (mode != JNI_COMMIT)                                                    \
      release(address);                                                        \
    return 0;                                                                  \
  }
  ARRAY_BUFFER(Boolean, jboolean, z)
  ARRAY_BUFFER(Byte, jbyte, b)
  ARRAY_BUFFER(Char, jchar, c) ARRAY_BUFFER(Short, jshort, s)
      ARRAY_BUFFER(Int, jint, i) ARRAY_BUFFER(Long, jlong, j)
          ARRAY_BUFFER(Float, jfloat, f) ARRAY_BUFFER(Double, jdouble, d)
#undef ARRAY_BUFFER
              uint64_t GetPrimitiveArrayCritical(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto array = c.take<jarray>();
    auto copy = c.take<jboolean *>();
    auto e = env();
    char primitive=0;
#if !defined(__ANDROID__)
    if(ti){
    auto type = e->GetObjectClass(array);
    char *descriptor = nullptr;
    auto error = ti->GetClassSignature(type, &descriptor, nullptr);
    e->DeleteLocalRef(type);
    if (error != JVMTI_ERROR_NONE || !descriptor)
      throw std::runtime_error("JVM array descriptor is unavailable");
    primitive = descriptor[1];
    ti->Deallocate(reinterpret_cast<unsigned char *>(descriptor));
    }else
#endif
    {
      for(char candidate:std::string_view("ZBCSIJFD")){
        const char name[]{'[',candidate,0};
        auto type=e->FindClass(name);
        if(!type)return 0;
        bool matches=e->IsInstanceOf(array,type);
        e->DeleteLocalRef(type);
        if(matches){primitive=candidate;break;}
      }
      if(!primitive)throw std::runtime_error("JNI array is not primitive");
    }
    size_t width = primitive == 'J' || primitive == 'D'   ? 8
                   : primitive == 'I' || primitive == 'F' ? 4
                   : primitive == 'C' || primitive == 'S' ? 2
                                                          : 1;
    auto count = e->GetArrayLength(array);
    auto retained = static_cast<jarray>(e->NewGlobalRef(array));
    auto pointer = e->GetPrimitiveArrayCritical(array, copy);
    return borrow(pointer, size_t(count) * width, retained,
                  [retained, pointer](JNIEnv *env) {
                    env->ReleasePrimitiveArrayCritical(retained, pointer, 0);
                  });
  }
  uint64_t ReleasePrimitiveArrayCritical(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto array = c.take<jarray>();
    auto address = c.word();
    auto mode = c.take<jint>();
    env()->ReleasePrimitiveArrayCritical(
        array, address ? memory.translate(address, 1, Memory::Read) : nullptr,
        mode);
    if (mode != JNI_COMMIT)
      release(address);
    return 0;
  }
  uint64_t NewDirectByteBuffer(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto address = c.word();
    auto size = c.take<jlong>();
    void *bytes = address
                      ? memory.translate(address, size < 0 ? 0 : size_t(size),
                                         Memory::Read | Memory::Write)
                      : nullptr;
    return returned(env()->NewDirectByteBuffer(bytes, size));
  }
  uint64_t GetDirectBufferAddress(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto object = c.take<jobject>();
    auto e = env();
    // Java owns these buffers. A weak owner removes obsolete address views
    // without preventing collection of temporary NIO buffers.
    std::vector<std::pair<uint32_t, jobject>> expired;
    {
      std::lock_guard lock(mutex);
      std::erase_if(buffers, [&](auto &entry) {
        auto &buffer = entry.second;
        if (!buffer.weak || !e->IsSameObject(buffer.owner, nullptr))
          return false;
        expired.emplace_back(entry.first, buffer.owner);
        return true;
      });
    }
    for (auto [address, owner] : expired) {
      memory.unmap(address);
      e->DeleteWeakGlobalRef(owner);
    }
    void *bytes = e->GetDirectBufferAddress(object);
    if (!bytes)
      return 0;
    auto address = memory.addressOf(bytes);
    if (address)
      return address;
    auto count = e->GetDirectBufferCapacity(object);
    if (count < 0)
      return 0;
    size_t width = 1;
    // JNI capacity uses elements, not bytes, for typed NIO views.
    const std::pair<const char *, size_t> elements[] = {
        {"java/nio/ByteBuffer", sizeof(jbyte)},
        {"java/nio/CharBuffer", sizeof(jchar)},
        {"java/nio/ShortBuffer", sizeof(jshort)},
        {"java/nio/IntBuffer", sizeof(jint)},
        {"java/nio/LongBuffer", sizeof(jlong)},
        {"java/nio/FloatBuffer", sizeof(jfloat)},
        {"java/nio/DoubleBuffer", sizeof(jdouble)}};
    for (auto [name, size] : elements) {
      auto type = e->FindClass(name);
      if (!type)
        return 0;
      bool match = e->IsInstanceOf(object, type);
      e->DeleteLocalRef(type);
      if (match) {
        width = size;
        break;
      }
    }
    if (uint64_t(count) > UINT32_MAX / width)
      throw std::runtime_error("JNI direct buffer exceeds ARM32 address space");
    return borrow(bytes, size_t(count) * width, e->NewWeakGlobalRef(object), {},
                  true);
  }
  uint64_t RegisterNatives(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto type = c.take<jclass>();
    auto address = c.word();
    auto count = c.take<jint>();
    auto e = env();
    if (count < 0)
      return uint64_t(e->RegisterNatives(type, nullptr, count));
    memory.translate(address, size_t(count) * 12, Memory::Read);
    std::vector<NativeMethod> methods;
    for (jint i = 0; i < count; i++) {
      auto raw = static_cast<const uint32_t *>(
          memory.translate(address + uint32_t(i) * 12, 12, Memory::Read));
      methods.push_back({memory.string(raw[0]), memory.string(raw[1]), raw[2]});
    }
    return uint64_t(bind(e, type, methods));
  }
  jint bind(JNIEnv *e, jclass type, std::span<const NativeMethod> entries) {
    Registration registration{static_cast<jclass>(e->NewGlobalRef(type)), {}};
    if (!registration.type)
      return JNI_ERR;
    std::vector<JNINativeMethod> methods;
    try {
      for (auto &entry : entries) {
        if(metadata.runtimeRegistration){
          methods.push_back({const_cast<char*>(entry.name.c_str()),const_cast<char*>(entry.signature.c_str()),
                             reinterpret_cast<void*>(uintptr_t(entry.function))});
          continue;
        }
        auto native = std::make_shared<Native>(*this, entry.name,
                                               entry.signature, entry.function);
        methods.push_back(
            {native->name.data(), native->signature.data(), native->code});
        registration.methods.push_back(std::move(native));
      }
      auto result =
          e->RegisterNatives(type, methods.data(), jint(methods.size()));
      if (result == JNI_OK) {
        std::lock_guard lock(mutex);
        registrations.push_back(std::move(registration));
      } else
        e->DeleteGlobalRef(registration.type);
      return uint64_t(result);
    } catch (...) {
      e->DeleteGlobalRef(registration.type);
      throw;
    }
  }
  void *trampoline(uint32_t function,std::string_view shorty,NativeCall callType){
    if(!function)throw std::runtime_error("JNI native function is null");
    auto signature=shortyDescriptor(shorty);
    const auto key=std::make_tuple(function,signature,callType);
    std::lock_guard lock(mutex);
    auto found=trampolines.find(key);
    if(found!=trampolines.end())return found->second->code;
    auto native=std::make_shared<Native>(*this,std::string{},std::move(signature),function,callType);
    auto code=native->code;trampolines.emplace(key,std::move(native));return code;
  }
  uint64_t invokeVm(Thread &thread,JNIEnv *env,uint32_t function,void *reserved){
    if(reserved)throw std::runtime_error("JNI invocation reserved argument must be null");
    Context context(*this,thread,env);
    const std::array<Value,2> arguments{Value::word(virtualVm),Value::word(0)};
    return thread.call(function,arguments);
  }
  uint64_t UnregisterNatives(Thread &t) {
    Cursor c(*this, t);
    c.word();
    auto type = c.take<jclass>();
    auto e = env();
    auto result = e->UnregisterNatives(type);
    if (result == JNI_OK) {
      std::lock_guard lock(mutex);
      std::erase_if(registrations, [&](auto &registration) {
        if (!e->IsSameObject(registration.type, type))
          return false;
        e->DeleteGlobalRef(registration.type);
        return true;
      });
    }
    return uint64_t(result);
  }
  jvalue call(Thread &thread, JNIEnv *e, uint32_t function, jobject receiver,
              std::string_view descriptor, std::span<const jvalue> values,bool critical=false) {
    auto types = parameterTypes(descriptor);
    if (types.size() != values.size())
      throw std::runtime_error(
          "JNI argument count differs from its descriptor");
    Context context(*this, thread, e);
    std::vector<Value> args;
    if(!critical)args={Value::word(environment),Value::word(handle(e->NewLocalRef(receiver)))};
    for (size_t i = 0; i < types.size(); i++) {
      uint64_t bits = 0;
      if (types[i] == 'L')
        bits = handle(e->NewLocalRef(values[i].l));
      else if (types[i] == 'B')
        bits = uint32_t(int32_t(values[i].b));
      else if (types[i] == 'S')
        bits = uint32_t(int32_t(values[i].s));
      else
        std::memcpy(&bits, &values[i], 8);
      args.push_back(types[i] == 'J' || types[i] == 'D'
                         ? Value::wide(bits)
                         : Value::word(uint32_t(bits)));
    }
    jvalue result{};
    auto bits = thread.call(function, args);
    char type = returnType(descriptor);
    if (type == 'L')
      result.l = context.finish(reference<jobject>(uint32_t(bits)));
    else {
      std::memcpy(&result, &bits, 8);
      context.finish();
    }
    return result;
  }
};
thread_local Jni::Impl::Context *Jni::Impl::current = nullptr;
thread_local Jni::Impl::ThreadOwners *Jni::Impl::threadOwners=nullptr;
Jni::Jni(Memory &memory, JavaVM *vm,void (*nativeThreadEntry)(),
         const KljJniInvocationApi *invocation)
    : Jni(memory,vm,Metadata{},nativeThreadEntry,invocation) {}
Jni::Jni(Memory &memory, JavaVM *vm,Metadata metadata,
         void (*nativeThreadEntry)(),const KljJniInvocationApi *invocation)
    : impl(std::make_unique<Impl>(memory, vm,nativeThreadEntry,invocation,metadata)) {}
Jni::~Jni() = default;
jint Jni::bind(JNIEnv *env, jclass type,
               std::span<const NativeMethod> methods) {
  return impl->bind(env, type, methods);
}
void *Jni::trampoline(uint32_t function,std::string_view shorty,NativeCall callType){
  return impl->trampoline(function,shorty,callType);
}
std::shared_ptr<Thread> Jni::thread() { return impl->guestThread(); }
void Jni::setSyscall(std::function<int64_t(Thread &,uint32_t)> handler) {
  std::lock_guard lock(impl->threadPool->mutex);
  impl->threadPool->syscall=std::move(handler);
  for(auto &[id,thread]:impl->threadPool->threads){(void)id;thread->setSyscall(impl->threadPool->syscall);}
}
uint64_t Jni::nativeValue(JniValue kind,uint64_t value,bool toHost) {
  if(!value)return 0;
  if(kind==JniValue::Environment){
    if(toHost&&value!=impl->environment)throw std::runtime_error("Invalid ARM32 JNI environment");
    return toHost?reinterpret_cast<uintptr_t>(impl->env()):impl->environment;
  }
  if(kind==JniValue::VM){
    if(toHost&&value!=impl->virtualVm)throw std::runtime_error("Invalid ARM32 JavaVM");
    return toHost?reinterpret_cast<uintptr_t>(impl->vm):impl->virtualVm;
  }
  if(toHost)return reinterpret_cast<uintptr_t>(impl->reference<void*>(uint32_t(value)));
  return impl->handle(reinterpret_cast<void*>(uintptr_t(value)),kind==JniValue::Object?Reference::Local:Reference::Opaque);
}
uint64_t Jni::callback(Thread &thread,uint32_t function,std::span<const Value> arguments) {
  JNIEnv *env=nullptr;
  impl->vm->GetEnv(reinterpret_cast<void **>(&env),JNI_VERSION_1_6);
  Impl::Context context(*impl,thread,env);
  return thread.call(function,arguments);
}
void Jni::construct(Thread &thread, JNIEnv *env, Image &image) {
  Impl::Invocation invocation(impl->invocation,env);
  if(!invocation.entered)return;
  Impl::Context context(*impl, thread, env);
  image.initialize(thread);
}
jint Jni::initialize(Thread &thread, JNIEnv *env, uint32_t onLoad) {
  Impl::Invocation invocation(impl->invocation,env);
  if(!invocation.entered)return JNI_ERR;
  return jint(impl->invokeVm(thread,env,onLoad,nullptr));
}
jvalue Jni::call(Thread &thread, JNIEnv *env, uint32_t function,
                 jobject receiver, std::string_view descriptor,
                 std::span<const jvalue> arguments) {
  return impl->call(thread, env, function, receiver, descriptor, arguments);
}
} // namespace maclauncher::arm32
