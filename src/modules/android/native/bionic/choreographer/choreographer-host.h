#pragma once
#include "ndk-host.h"
#include "call-packet.h"
#include "callback-owner.h"
#include <android/choreographer.h>
#define KLJ_BIONIC_CHOREOGRAPHER_HOSTCALL UINT32_C(0x43485201)
typedef struct {
    void *(*resolve)(const char *);
    int64_t (*invoke)(void *,JNIEnv *,uintptr_t,KljCallPacket *);
    void *context;
    JavaVM *vm;
} KljBionicChoreographerOwner;
int klj_bionic_choreographer_prepare(const KljBionicChoreographerOwner *);
const KljBionicNdkBindings *klj_bionic_choreographer_bindings(void);
int64_t klj_bionic_choreographer_dispatch(JNIEnv *,KljBionicNdkPacket *);
uint64_t klj_bionic_choreographer_install(uint64_t);
