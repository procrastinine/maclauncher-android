#pragma once
#include <stddef.h>
#include <stdint.h>
#include "preconstructor-abi.h"
#include "dlopen-observer.h"
#define KLJ_BIONIC_LINKER_HOSTCALL UINT32_C(0x4c4e4b01)
#define KLJ_BIONIC_RUNTIME_ABI 1u
#define KLJ_BIONIC_BOOTSTRAP_HOSTCALL UINT32_C(0x4a425354)
#define KLJ_BIONIC_PREPARED_HOSTCALL UINT32_C(0x4a505245)
typedef struct {
 uint32_t size,version;
 uint64_t prepare,join,packet,jni_install,jni_tables;
 uint64_t open_prepared,symbol,close_image,try_join_coordinator;
 uint64_t pid,tid,coordinator_tid,prepared_entry;
 MaclauncherDlopenImageV1 helper;
} KljBionicGuest;
typedef struct {uint64_t token,thread;int32_t error;uint32_t reserved;} KljBionicThreadRequest;
typedef struct {
 uint32_t size,version;const char *filename;int32_t flags;uint32_t reserved;
 const void *caller;void *context;void *handle;char *error;size_t error_capacity;
} KljBionicOpen;
typedef struct {
 uint32_t size,version;void *handle;const char *name;void *symbol;
 char *error;size_t error_capacity;
} KljBionicSymbol;
typedef struct {uint32_t size,version;void *handle;char *error;size_t error_capacity;} KljBionicCloseImage;
typedef struct {
 uint32_t size,version;void *context;const MaclauncherDlopenGraphV1 *graph;
 char *error;size_t error_capacity;
} KljBionicPreparedEvent;
_Static_assert(sizeof(KljBionicThreadRequest)==24,"Bionic thread request ABI");
_Static_assert(sizeof(KljBionicGuest)==216,"Bionic guest bootstrap ABI");
_Static_assert(sizeof(KljBionicOpen)==64,"Bionic open request ABI");
_Static_assert(sizeof(KljBionicSymbol)==48,"Bionic symbol request ABI");
_Static_assert(sizeof(KljBionicCloseImage)==32,"Bionic close request ABI");
_Static_assert(sizeof(KljBionicPreparedEvent)==40,"Bionic prepared event ABI");

typedef struct KljBionicLinkerEvent {
 uint32_t size,version,phase,reserved;const void *caller;
 const MaclauncherDlopenGraphV1 *graph;void *handle;uintptr_t bias;
 char *error;size_t error_capacity;
} KljBionicLinkerEvent;
_Static_assert(sizeof(KljBionicLinkerEvent)==64,"Bionic linker lifecycle event ABI");
