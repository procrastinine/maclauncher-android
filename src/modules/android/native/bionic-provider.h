#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Synchronous operations on an already initialized Bionic thread. The caller
 * retains its existing host-call frame, linker namespace and thread owner.
 * No pointer from this scope may be used on a foreign or detached thread. */
typedef uint64_t (*KljBionicGuestCallback)(uint64_t function,uint64_t argument);
typedef struct { KljBionicGuestCallback enter; uint64_t dispatcher; } KljBionicProvider;
/* Original Android dlfcn flags. Darwin RTLD_LOCAL/RTLD_GLOBAL/RTLD_NOLOAD
 * have different numeric values and must not cross this boundary. */
enum { KLJ_BIONIC_RTLD_LOCAL=0,KLJ_BIONIC_RTLD_LAZY=1,KLJ_BIONIC_RTLD_NOW=2,
       KLJ_BIONIC_RTLD_NOLOAD=4,KLJ_BIONIC_RTLD_GLOBAL=0x100,KLJ_BIONIC_RTLD_NODELETE=0x1000 };
enum { KLJ_BIONIC_OPEN=1,KLJ_BIONIC_SYMBOL=2,KLJ_BIONIC_CLOSE=3 };
typedef struct {
 uint64_t operation,handle,text,flags,result;
 char error[512];
} KljBionicLinkRequest;
/* The callbacks execute the original Bionic dlfcn implementations. Handles
 * remain owned by that linker; symbol addresses name its real ELF mappings. */
int klj_bionic_open(const KljBionicProvider *,const char *,int,void **,char *,size_t);
int klj_bionic_symbol(const KljBionicProvider *,void *,const char *,void **,char *,size_t);
int klj_bionic_close(const KljBionicProvider *,void *,char *,size_t);
uint64_t klj_bionic_link_dispatch(uint64_t);
#ifdef __cplusplus
}
#endif
