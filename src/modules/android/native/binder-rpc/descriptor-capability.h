#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __ANDROID__
#include "bridge.h"
struct KljArtDescriptorCapability {uint32_t version,operation;int32_t descriptor;uint32_t reserved;uint64_t bytes,size;};
static inline int64_t klj_ashmem_capability_export(int fd,uint8_t* token,size_t capacity){
    KljArtDescriptorCapability value{1,0,fd,0,(uintptr_t)token,capacity};
    return klj_art_host(15,&value,sizeof(value));
}
static inline int klj_ashmem_capability_import(int fd,const uint8_t* token,size_t size){
    KljArtDescriptorCapability value{1,1,fd,0,(uintptr_t)token,size};
    return (int)klj_art_host(15,&value,sizeof(value));
}
#else
extern "C" int64_t klj_ashmem_capability_export(int,uint8_t*,size_t);
extern "C" int klj_ashmem_capability_import(int,const uint8_t*,size_t);
#endif
