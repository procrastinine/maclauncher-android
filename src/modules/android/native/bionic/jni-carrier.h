#pragma once
#include <jni.h>
#include <stddef.h>
#include <stdint.h>
#include "jni-invocation.h"
#define KLJ_BIONIC_JNI_ABI 1u
#define KLJ_BIONIC_JNI_HOSTCALL UINT32_C(0x4a4e4901)
typedef struct {
 uint32_t size,version;
 uint64_t target,argument,argument_size,caller;
} KljBionicJniPacket;
typedef struct {uint64_t function,argument_size;} KljBionicJniTarget;
typedef struct {
 uint32_t size,version,jni_count,vm_count;
 const KljBionicJniTarget *jni,*vm;
} KljBionicJniBindings;
typedef struct {
 const struct JNINativeInterface_ *jni;
 const struct JNIInvokeInterface_ *vm;
} KljBionicJniGuestTables;
typedef struct KljBionicJniOwner KljBionicJniOwner;
KljBionicJniOwner *klj_bionic_jni_owner(JNIEnv *,const KljBionicJniGuestTables *,int(*prepare_attach)(void *),void *);
JNIEnv *klj_bionic_jni_environment(KljBionicJniOwner *,JNIEnv *real_environment);
JavaVM *klj_bionic_jni_vm(KljBionicJniOwner *);
int64_t klj_bionic_jni_host_call(KljBionicJniOwner *,KljBionicJniPacket *);
uintptr_t klj_bionic_jni_caller(void);
typedef struct {
 uint64_t x[9],lr;unsigned char vectors[8][16];
 uint64_t stack,integer,floating,operation,reserved[2];
} KljBionicJniVariadicFrame;
_Static_assert(sizeof(KljBionicJniVariadicFrame)==256,"complete JNI variadic frame");
_Static_assert(sizeof(KljBionicJniPacket)==40,"generic JNI carrier packet");
/* Generated transport covers every declaration from the selected original
 * jni.h, including all five JavaVM operations. No operation subset is selected. */
const KljBionicJniBindings *klj_bionic_jni_host_bindings(void);
int64_t klj_bionic_jni_dispatch_packet(KljBionicJniPacket *);
uint64_t klj_bionic_jni_guest_install(uint64_t trusted_bindings);
uint64_t klj_bionic_jni_guest_tables(uint64_t unused);
/* The embedding image/thread owner supplies these representation operations.
 * Each callback executes only inside the original Bionic host-call transition
 * on the same physical thread, with real host TLS and host stack installed. */
JNIEnv *klj_bionic_jni_host_environment(JNIEnv *);
JavaVM *klj_bionic_jni_host_vm(JavaVM *);
JNIEnv *klj_bionic_jni_wrap_environment(JNIEnv *);
JavaVM *klj_bionic_jni_wrap_vm(JavaVM *);
jclass klj_bionic_jni_find_class(JNIEnv *,const char *,uintptr_t original_caller);
/* A scope owns only one Attach operation and restores its predecessor even on
 * a JNI exceptional return. It never replaces process-wide attachment policy. */
int klj_bionic_jni_prepare_attach(void *);
