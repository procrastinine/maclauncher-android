#pragma once
#include <jni.h>
#include <stddef.h>
#include <stdint.h>
#define KLJ_BIONIC_NDK_HOSTCALL UINT32_C(0x4e444b01)
#define KLJ_BIONIC_NDK_ABI 1u
typedef struct {
 uint32_t size,version;
 uint64_t target,arguments,argument_size,caller;
 int32_t error,descriptor_socket;
} KljBionicNdkPacket;
typedef struct { uint64_t target,argument_size; } KljBionicNdkTarget;
typedef struct {
 uint32_t size,version,count,reserved;
 const KljBionicNdkTarget *targets;
} KljBionicNdkBindings;
typedef struct {
 void *(*resolve)(const char *);
 JNIEnv *(*environment)(void *,JNIEnv *,JNIEnv *);
 void *context;
 int64_t (*read_lease)(void *,int,int *,void **);
 void (*release_lease)(void *,void *);
} KljBionicNdkOwner;
/* Both generated sides compile the original published declarations. Opaque
 * resource values remain owned by the original host implementation. */
const KljBionicNdkBindings *klj_bionic_ndk_bindings(void);
int klj_bionic_ndk_prepare(const KljBionicNdkOwner *);
int64_t klj_bionic_ndk_dispatch(JNIEnv *,KljBionicNdkPacket *);
uint64_t klj_bionic_ndk_install(uint64_t);
int klj_bionic_ndk_send_descriptor(int socket,int descriptor);
_Static_assert(sizeof(KljBionicNdkPacket)==48,"NDK host-call transport ABI");
