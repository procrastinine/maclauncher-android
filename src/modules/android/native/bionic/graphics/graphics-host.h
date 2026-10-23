#pragma once
#include "ndk-host.h"
#include "call-packet.h"
#define KLJ_BIONIC_GRAPHICS_HOSTCALL UINT32_C(0x47465801)
typedef struct {
 void *(*resolve)(const char *);
 JNIEnv *(*environment)(void *,JNIEnv *,JNIEnv *);
 int64_t (*invoke)(void *,JNIEnv *,uintptr_t,KljCallPacket *);
 void *context;
} KljBionicGraphicsOwner;
const KljBionicNdkBindings *klj_bionic_graphics_bindings(void);
int klj_bionic_graphics_prepare(const KljBionicGraphicsOwner *);
int64_t klj_bionic_graphics_dispatch(JNIEnv *,KljBionicNdkPacket *);
uint64_t klj_bionic_graphics_install(uint64_t);
