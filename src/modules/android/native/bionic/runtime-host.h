#pragma once
#include <jni.h>
#include <stddef.h>
#include <stdint.h>
#include "runtime-protocol.h"
#include "call-packet.h"
#include "jni-carrier.h"
typedef struct KljBionicRuntime KljBionicRuntime;
typedef struct {
 uint32_t size,version;
 const char *abi,*root,*path_map,*program,*runtime,*host_module,*identity;
 const char *const *envp;size_t envc;
} KljBionicRuntimeOptions;
typedef int (*KljBionicPreparedCallback)(void *owner,KljBionicRuntime *,JNIEnv *,
 void *request_context,const MaclauncherDlopenGraphV1 *,char *error,size_t error_capacity,
 const KljBionicLinkerEvent *linker_event);
typedef int64_t (*KljBionicHostCallback)(void *owner,KljBionicRuntime *,JNIEnv *,
 uint32_t function,uint64_t argument,uint64_t length);
int64_t klj_bionic_runtime_host_callback(KljBionicRuntime *,KljBionicHostCallback,void *);
/* Explicit prepare on the physical Java first thread. Original initialized
 * libjsig and the normal preloads must already be present. Nothing is selected
 * or downloaded by this API. All records/code retain process lifetime.
 * Status returns zero or a negative Linux errno; no operation sets a pending
 * Java exception merely to translate an ordinary linker/admission error. */
int64_t klj_bionic_runtime_prepare(JNIEnv *,const KljBionicRuntimeOptions *,KljBionicRuntime **);
const KljBionicGuest *klj_bionic_runtime_guest(KljBionicRuntime *);
KljBionicJniOwner *klj_bionic_runtime_jni_owner(KljBionicRuntime *);
/* Both calls preserve raw argument representation. In particular packet never
 * remaps x0: the typed JNI image owner supplies outer Env or outer VM as needed.
 * Returning entry scopes have no setjmp continuation across live Rust frames. */
int64_t klj_bionic_runtime_invoke(KljBionicRuntime *,JNIEnv *,uint64_t function,uint64_t argument,uint64_t *result);
int64_t klj_bionic_runtime_packet(KljBionicRuntime *,JNIEnv *,KljCallPacket *);
/* Called only after the image owner authenticates a live native callback's
 * caller and target closure. A fresh host feeder acquires original Bionic
 * pthread state without attaching to Java and retires it at physical exit. */
int64_t klj_bionic_runtime_native_packet(KljBionicRuntime *,KljCallPacket *);
int64_t klj_bionic_runtime_open(KljBionicRuntime *,JNIEnv *,KljBionicOpen *);
int64_t klj_bionic_runtime_symbol(KljBionicRuntime *,JNIEnv *,KljBionicSymbol *);
int64_t klj_bionic_runtime_close_image(KljBionicRuntime *,JNIEnv *,KljBionicCloseImage *);
int64_t klj_bionic_runtime_prepared_callback(KljBionicRuntime *,KljBionicPreparedCallback,void *owner);
int64_t klj_bionic_runtime_status(KljBionicRuntime *);
/* First-thread-only nonblocking admission-service drain. EBUSY preserves the
 * service while ordinary admitted threads/entries/requests remain or the
 * original coordinator pthread is still retiring. No other guest worker is
 * killed, no image/ABI is unloaded and the initial process nonce is retained. */
int64_t klj_bionic_runtime_try_close(KljBionicRuntime *,JNIEnv *);
