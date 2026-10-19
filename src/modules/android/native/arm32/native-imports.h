#pragma once
#include "arm32.h"
#include "jni-value.h"
#include <array>
#include <functional>
#include <memory>

namespace maclauncher::arm32 {
// Representation adapter only: all operations are supplied by the same ARM64
// libc/NDK/GLES owners used by native Android libraries.
class Imports {
public:
  Imports(Memory &, void *(*lookup)(const char *));
  ~Imports();
  void setThreadFactory(std::function<std::shared_ptr<Thread>()>);
  void setCallback(std::function<uint64_t(Thread &,uint32_t,std::span<const Value>)>);
  void setJniAdapter(std::function<uint64_t(JniValue,uint64_t,bool)>);
  void setThreadExitProbe(int (*)(void));
  void retireThread(void *);
  uint32_t caller() const;
  uint32_t resolve(std::string_view);
  uint32_t resolve(std::string_view,void *native);
  int64_t syscall(Thread &,uint32_t number);
private:
  int64_t kernelCall(Thread &,uint32_t,const std::array<uint32_t,7> &);
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}
