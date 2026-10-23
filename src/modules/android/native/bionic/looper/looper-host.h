#pragma once
#include "ndk-host.h"
#include "call-packet.h"
#include "poll-fd.h"
#include <android/looper.h>
#define KLJ_BIONIC_LOOPER_HOSTCALL UINT32_C(0x4c4f4f01)
typedef struct{
 void *(*resolve)(const char*);
 int64_t(*invoke)(void*,JNIEnv*,uintptr_t,KljCallPacket*);
 void *context;
 const KljBionicPollFdApi *poll;
}KljBionicLooperOwner;
int klj_bionic_looper_prepare(JNIEnv*,const KljBionicLooperOwner*);
const KljBionicNdkBindings *klj_bionic_looper_bindings(void);
int64_t klj_bionic_looper_dispatch(JNIEnv*,KljBionicNdkPacket*);
uint64_t klj_bionic_looper_install(uint64_t);
/* Original JVMTI owner calls javaEnd; this explicit operation also permits
 * orderly shutdown while the current JavaThread is still attached. */
void klj_bionic_looper_retire_current(void);
