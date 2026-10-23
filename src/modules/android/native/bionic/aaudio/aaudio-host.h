#pragma once
#include "ndk-host.h"
#include "call-packet.h"
#include "callback-owner.h"
#ifndef __ANDROID__
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api)
#endif
#endif
#include <aaudio/AAudio.h>
#define KLJ_BIONIC_AAUDIO_HOSTCALL UINT32_C(0x41415501)
typedef struct {
    void *(*resolve)(const char *);
    int64_t (*invoke)(void *,JNIEnv *,uintptr_t,KljCallPacket *);
    void *context;
    JavaVM *vm;
} KljBionicAAudioOwner;
int klj_bionic_aaudio_prepare(const KljBionicAAudioOwner *);
const KljBionicNdkBindings *klj_bionic_aaudio_bindings(void);
int64_t klj_bionic_aaudio_dispatch(JNIEnv *,KljBionicNdkPacket *);
uint64_t klj_bionic_aaudio_install(uint64_t);
