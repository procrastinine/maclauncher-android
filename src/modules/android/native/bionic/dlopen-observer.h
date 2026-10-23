/* The original linker owns the borrowed graph and all constructor/unload order. */
#pragma once
#include <stddef.h>
#include <stdint.h>
struct MaclauncherDlopenGraphV1;
#define MACLAUNCHER_DLOPEN_OBSERVER_ABI 1u
#define MACLAUNCHER_DLOPEN_PREPARE 1u
#define MACLAUNCHER_DLOPEN_COMPLETE 2u
#define MACLAUNCHER_DLOPEN_RETIRE 3u
typedef struct MaclauncherDlopenObserverV1 {
 uint32_t size,abi;void *context;
 int (*event)(void *,uint32_t,const void *,const struct MaclauncherDlopenGraphV1 *,
              void *,uintptr_t,char *,size_t);
} MaclauncherDlopenObserverV1;
