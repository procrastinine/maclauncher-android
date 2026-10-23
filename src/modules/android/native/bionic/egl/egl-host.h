#pragma once
#include "ndk-host.h"
#include "call-packet.h"
#include "../../angle-debug-callback.h"
#define KLJ_BIONIC_EGL_HOSTCALL UINT32_C(0x45474c01)
/* resolve is the original, already configured klj_native_egl_lookup export
 * from libklepton-graphics. Its native surface and ANGLE owners remain intact.
 * klj_native_egl_guest_lookup/legacy ABI thunks are not native C callees. */
typedef struct {
 void *(*resolve)(const char *);
 void (*error)(unsigned); /* Same supplier's klj_native_egl_error. */
 int64_t (*invoke)(void *,JNIEnv *,uintptr_t,KljCallPacket *);
 void *context;
 KljAngleDebugOwnerInstall debug_callback;
    JavaVM *vm;
} KljBionicEglOwner;
int klj_bionic_egl_prepare(const KljBionicEglOwner *);
const KljBionicNdkBindings *klj_bionic_egl_bindings(void);
int64_t klj_bionic_egl_dispatch(JNIEnv *,KljBionicNdkPacket *);
uint64_t klj_bionic_egl_install(uint64_t);
/* The original host provider, current thread, and error state are shared.
 * Only procedure addresses change ownership: an available, declared function
 * resolves to its genuine ELF veneer, never to the provider's Darwin PC. */
