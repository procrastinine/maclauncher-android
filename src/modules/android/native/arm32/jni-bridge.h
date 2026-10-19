#pragma once
#include "arm32.h"
#include "jni-value.h"
#include <jni.h>
#include "../jni-invocation.h"

namespace maclauncher::arm32 {
// A width adapter for the executing VM, never a second JNI implementation.
// All function slots are generated from that VM's original jni.h.
class Jni {
public:
  struct Metadata {
    // The original ART NativeBridgeRuntimeCallbacks method-shorty contract.
    // References, JNI calls and their lifetime remain in this same adapter.
    const char *(*methodShorty)(JNIEnv *, jmethodID);
    // NativeBridge v8 lets original ART choose each registered method's JNI
    // call type before requesting this adapter's shared native trampoline.
    bool runtimeRegistration = false;
  };
  Jni(Memory &, JavaVM *, void (*nativeThreadEntry)() = nullptr,
      const KljJniInvocationApi * = nullptr);
  Jni(Memory &, JavaVM *, Metadata,
      void (*nativeThreadEntry)() = nullptr,
      const KljJniInvocationApi * = nullptr);
  ~Jni();
  Jni(const Jni &) = delete;
  Jni &operator=(const Jni &) = delete;
  struct NativeMethod {
    std::string name, signature;
    uint32_t function;
  };
  jint bind(JNIEnv *, jclass, std::span<const NativeMethod>);
  enum class NativeCall { Regular, Critical, Invocation };
  void *trampoline(uint32_t function, std::string_view shorty,
                   NativeCall = NativeCall::Regular);
  void construct(Thread &, JNIEnv *, Image &);
  std::shared_ptr<Thread> thread();
  void setSyscall(std::function<int64_t(Thread &,uint32_t)>);
  uint64_t callback(Thread &,uint32_t,std::span<const Value>);
  uint64_t nativeValue(JniValue,uint64_t,bool toHost);
  jint initialize(Thread &, JNIEnv *, uint32_t onLoad);
  jvalue call(Thread &, JNIEnv *, uint32_t function, jobject receiver,
              std::string_view descriptor, std::span<const jvalue> arguments);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
} // namespace maclauncher::arm32
