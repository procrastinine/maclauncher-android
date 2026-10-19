#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct KljArm32CodeLocation {
  uint64_t cacheBegin, cacheEnd, entry, size, descriptor;
};
typedef int (*KljArm32CodeQuery)(void *, uint64_t,
                               struct KljArm32CodeLocation *);
/* 0: outside this owner's caches; 1: original block; 2: metadata busy;
 * 3: cache prelude or no current block. Queries never execute guest code. */
int maclauncher_arm32_code_location(uint64_t,
                                   struct KljArm32CodeLocation *);

#if defined(__GNUC__)
#define KLJ_CODE_INTERNAL __attribute__((visibility("hidden")))
#else
#define KLJ_CODE_INTERNAL
#endif
KLJ_CODE_INTERNAL void maclauncher_dynarmic_install_code_query(
    uint64_t, KljArm32CodeQuery, void *);
KLJ_CODE_INTERNAL void *maclauncher_dynarmic_enter_code_metadata(uint64_t);
KLJ_CODE_INTERNAL void maclauncher_dynarmic_leave_code_metadata(void *);
#undef KLJ_CODE_INTERNAL

#ifdef __cplusplus
}
struct KljArm32CodeMetadataScope {
  void *lock;
  explicit KljArm32CodeMetadataScope(uint64_t cache)
      : lock(maclauncher_dynarmic_enter_code_metadata(cache)) {}
  ~KljArm32CodeMetadataScope() {
    maclauncher_dynarmic_leave_code_metadata(lock);
  }
  KljArm32CodeMetadataScope(const KljArm32CodeMetadataScope &) = delete;
  KljArm32CodeMetadataScope &operator=(const KljArm32CodeMetadataScope &) = delete;
};
#endif
