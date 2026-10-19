#pragma once
#include <stdint.h>
#include <stddef.h>
#define KLJ_ART_MODULE 0x4d4c
#define KLJ_ART_SYSCALL 0x48430000
/* 12/13 are the kernel transport-peer/package-path operations. */
#define KLJ_ART_LOG_FUNCTION 14
typedef uint64_t (*KljArtCallback)(uint64_t function, uint64_t argument);
struct KljArtWindow {uint32_t operation,width,height,out_width,out_height,screen;float refresh,density;char error[256];};
struct KljArtFrame {uint64_t pixels,length;uint32_t width,height,stride,format;uint64_t frame,completed,presented;char error[512];};
/* Kernel-authenticated transport fields; Android identity is a separate lease. */
struct KljArtPeerCredentials {int32_t descriptor,androidPid,hostPid;uint32_t hostUid,hostGid;};
struct KljArtNetwork {uint32_t operation,allowed;uint64_t token,snapshot,buffer,capacity,length;};
struct KljArtLog {int32_t priority;uint32_t reserved;uint64_t tag,message;};
// Network result errors use the guest Linux ABI, not Darwin errno numbers.
#define KLJ_ART_NETWORK_ECANCELED 125
#define KLJ_ART_NETWORK_EILSEQ 84
#if defined(__ANDROID__) && defined(__aarch64__)
static inline int64_t klj_art_host(uint32_t function,void *packet,size_t size) {
 register uint64_t x0 __asm__("x0")=KLJ_ART_MODULE,x1 __asm__("x1")=function,x2 __asm__("x2")=(uintptr_t)packet,x3 __asm__("x3")=size,x8 __asm__("x8")=KLJ_ART_SYSCALL;
 __asm__ volatile("svc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3) : "r"(x8) : "x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","v0","v1","v2","v3","v4","v5","v6","v7","v16","v17","v18","v19","v20","v21","v22","v23","v24","v25","v26","v27","v28","v29","v30","v31","memory","cc");
 return (int64_t)x0;
}
#endif
