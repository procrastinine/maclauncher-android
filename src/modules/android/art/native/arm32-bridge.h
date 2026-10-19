#pragma once
#include "bridge.h"

/* CPU and address-space operations only. ELF dependency discovery, symbol
 * policy and JNI dispatch remain with their existing original owners. */
enum KljArtArm32Operation {
 KLJ_ART_ARM32_MEMORY=1, KLJ_ART_ARM32_THREAD, KLJ_ART_ARM32_MAP,
 KLJ_ART_ARM32_UNMAP, KLJ_ART_ARM32_IMPORT, KLJ_ART_ARM32_CALL,
 KLJ_ART_ARM32_CLOSE,
 KLJ_ART_ARM32_ALLOCATE, KLJ_ART_ARM32_PROTECT, KLJ_ART_ARM32_TRANSLATE,
 KLJ_ART_ARM32_LOAD_ADDRESS, KLJ_ART_ARM32_BORROW, KLJ_ART_ARM32_ADDRESS,
 KLJ_ART_ARM32_STRING, KLJ_ART_ARM32_GATE, KLJ_ART_ARM32_RETIRE_GATE,
 KLJ_ART_ARM32_ARGUMENT, KLJ_ART_ARM32_SYSCALL_ARGUMENT,
 KLJ_ART_ARM32_TLS, KLJ_ART_ARM32_SET_TLS, KLJ_ART_ARM32_SET_SYSCALL,
 KLJ_ART_ARM32_IMAGE, KLJ_ART_ARM32_IMAGE_INITIALIZE,
 KLJ_ART_ARM32_IMAGE_SYMBOL, KLJ_ART_ARM32_IMAGE_BASE,
 KLJ_ART_ARM32_IMAGE_SPAN, KLJ_ART_ARM32_IMAGE_HEADERS,
 KLJ_ART_ARM32_CALLBACK,
 KLJ_ART_ARM32_MAP_NATIVE, KLJ_ART_ARM32_RETIRE_NATIVE,
 KLJ_ART_ARM32_INVALIDATE, KLJ_ART_ARM32_MAPPING_DESCRIPTION,
 KLJ_ART_ARM32_IMAGE_INSPECT, KLJ_ART_ARM32_IMAGE_RELOCATE,
 KLJ_ART_ARM32_RETURN_ADDRESS, KLJ_ART_ARM32_IMAGE_RUNPATH,
 KLJ_ART_ARM32_ARGUMENTS, KLJ_ART_ARM32_MAPPINGS
};
struct KljArtArm32Value {uint64_t bits;uint32_t words,alignment;};
struct KljArtArm32Callback {uint64_t cookie,words;uint32_t count,reserved;};
struct KljArtArm32 {
 uint32_t operation,permission;
 uint64_t token,parent,buffer,function,cookie,result,budget;
 uint32_t address,length,count,reserved;
 char error[256];
};
#ifdef __cplusplus
extern "C" {
#endif
int64_t maclauncher_art_arm32(uint64_t,uint64_t,KljArtCallback);
#ifdef __cplusplus
}
#endif
