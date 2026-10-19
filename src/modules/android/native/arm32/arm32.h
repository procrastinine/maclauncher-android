#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace maclauncher::arm32 {
class Thread;
class Memory {
public:
  class Fault : public std::runtime_error { public: using std::runtime_error::runtime_error; };
  using HostCall = std::function<uint64_t(Thread &)>;
  enum Permission : unsigned { Execute = 1, Write = 2, Read = 4 };
  Memory();
  ~Memory();
  Memory(const Memory &) = delete;
  Memory &operator=(const Memory &) = delete;
  void map(uint32_t address, uint32_t size, unsigned permission);
  uint32_t allocate(uint32_t preferred, uint32_t size, unsigned permission);
  void unmap(uint32_t address);
  void unmap(uint32_t address, uint32_t size);
  void protect(uint32_t address, uint32_t size, unsigned permission);
  void *translate(uint32_t address, size_t size, unsigned permission) const;
  void *loadAddress(uint32_t address, size_t size) const;
  std::string string(uint32_t address) const;
  void observe(void *owner,
               std::function<void(uint32_t, uint32_t)> invalidation);
  void stopObserving(void *owner);
  void invalidate(uint32_t address, uint32_t size);
  // Process-owned call gates remain callable from every guest thread. JNI's
  // JavaVM table and native library imports belong to this shared address
  // space.
  uint32_t import(HostCall call);
  void retireImport(uint32_t address);
  uint64_t invoke(unsigned index, Thread &thread);
  uint32_t returnGate();
  // Borrowed JNI buffers retain their original JVM allocation and lifetime;
  // only their address representation changes for the 32-bit guest.
  uint32_t borrow(void *bytes, uint32_t size, unsigned permission,
                  std::string_view origin={});
  // Native mappings retain their one backing owner across guest page splits.
  // ARM32 page protection belongs to translated memory, independent of the
  // host page size and executable-code policy.
  uint32_t mapNative(void *bytes, uint32_t size, unsigned permission,
                     std::shared_ptr<void> owner, uint32_t preferred=0,
                     bool fixed=false, bool noReplace=false,
                     std::function<void(unsigned)> prepareProtection={},bool shared=false);
  uint32_t addressOf(const void *bytes) const;
  std::string mappingDescription(uint32_t address) const;
  struct Mapping {uint64_t start,end;unsigned permission,shared;};
  // Copy logical extents under this address space's read guard. The caller
  // formats and publishes the snapshot after the guard has been released.
  std::vector<Mapping> mappings() const;
  // The physical resource owner releases native storage. Retire every alias
  // of that extent through the existing native-range index first.
  void retireNative(const void *bytes,size_t size);

private:
  friend class Thread;
  friend class Image;
  struct Impl;
  std::unique_ptr<Impl> impl;
};
struct Value {
  uint64_t bits;
  unsigned words;
  unsigned alignment = 0;
  static Value word(uint32_t value) { return {value, 1, 4}; }
  static Value wide(uint64_t value) { return {value, 2, 8}; }
};
class Thread {
public:
  using HostCall = std::function<uint64_t(Thread &)>;
  explicit Thread(Memory &memory, uint32_t stackBase = 0x70000000,
                  uint32_t stackSize = 1024 * 1024);
  ~Thread();
  Thread(const Thread &) = delete;
  Thread &operator=(const Thread &) = delete;
  uint32_t import(HostCall call);
  uint64_t call(uint32_t address, std::span<const Value> arguments = {},
                uint64_t instructionBudget = UINT64_MAX);
  uint32_t argument(unsigned word) const;
  void arguments(unsigned first, std::span<uint32_t> words) const;
  uint32_t syscallArgument(unsigned word) const;
  uint32_t returnAddress() const;
  uint32_t tls() const;
  void setTls(uint32_t address);
  Memory &memory();
  // Linux EABI SVC: number in r7, seven argument words in r0-r6. The owner translates
  // 32-bit guest structs/pointers before invoking shared host services.
  void setSyscall(std::function<int64_t(Thread &, uint32_t)> call);

private:
  friend class Image;
  struct Impl;
  std::unique_ptr<Impl> impl;
};
class Image {
public:
  struct Layout { uint32_t span; std::vector<std::string> needed; };
  static Layout inspect(std::span<const uint8_t> bytes);
  using Resolve = std::function<uint32_t(std::string_view)>;
  // Map first so one loader transaction can resolve a cyclic DT_NEEDED group
  // before publishing any image or entering its constructors.
  Image(Memory &memory, std::span<const uint8_t> bytes, uint32_t loadBias);
  Image(Memory &memory, std::span<const uint8_t> bytes, uint32_t loadBias,
        Resolve resolver);
  void relocate(Resolve resolver);
  ~Image();
  uint32_t symbol(std::string_view name) const;
  void initialize(Thread &thread);
  const std::vector<std::string> &dependencies() const;
  std::string runpath() const;
  uint32_t base() const;
  uint32_t span() const;
  const void *programHeaders(unsigned *count) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
} // namespace maclauncher::arm32
