/* Additive prepared-dlopen ABI; borrowed graph lifetime is the synchronous callback. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MACLAUNCHER_DLOPEN_PREPARE_ABI 1u
#define MACLAUNCHER_DLOPEN_IMAGE_ROOT 1u
#define MACLAUNCHER_DLOPEN_IMAGE_CONSTRUCTED 2u

/* Borrowed original linker identities. Valid only during prepare, while the
 * original recursive loader lock and the new dlopen reference are retained.
 * No constructor address or dependency order is replaced by this interface. */
typedef struct MaclauncherDlopenImageV1 {
  uint32_t size;
  uint32_t flags;
  void* handle;
  uintptr_t mapping_base;
  size_t mapping_size;
  uintptr_t load_bias;
  const char* name;
  const void* phdr;
  size_t phnum;
  const size_t* dependencies;
  size_t dependency_count;
  uint64_t device;
  uint64_t inode;
  int64_t file_offset;
} MaclauncherDlopenImageV1;

typedef struct MaclauncherDlopenGraphV1 {
  uint32_t size;
  uint32_t abi;
  size_t image_count;
  const MaclauncherDlopenImageV1* images;
  size_t root_index;
} MaclauncherDlopenGraphV1;

/* A successful callback publishes/binds its existing loader transaction before
 * returning1. A rejected callback returns0 only after rolling back its own
 * bindings and borrowed addresses. The linker then releases its new reference
 * through original soinfo_unload, without invoking new constructors.
 * The callback must not unwind/longjmp across linker frames, close borrowed
 * handles, or recursively load an image in this graph before its preparation
 * is complete. Unrelated ordinary dlopen/dlsym retain original lock semantics.
 * This is synchronous, not a deferred-constructor handle API. */
typedef struct MaclauncherDlopenPrepareV1 {
  uint32_t size;
  uint32_t abi;
  void* context;
  int (*prepare)(void* context, const MaclauncherDlopenGraphV1* graph,
                 char* error, size_t error_capacity);
} MaclauncherDlopenPrepareV1;

