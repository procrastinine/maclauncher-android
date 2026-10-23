#pragma once
#include <jni.h>
#include <stdint.h>
#include <stddef.h>
/* Called by an actual linked-image owner before constructors. The opaque
 * identity stays with that owner; it is never interpreted as another loader's
 * image handle. Program headers describe original mapped ELF64 segments. */
typedef struct KljJniImage KljJniImage;
typedef struct {
 uint32_t type,flags;
 uint64_t offset,vaddr,paddr,filesz,memsz,align;
} KljJniProgramHeader;
typedef void *(*KljJniImageABI)(void *owner,void *target,const char *descriptor,char *error,size_t capacity);
KljJniImage *klj_jni_image_publish(JNIEnv *,const void *identity,uintptr_t load_bias,
 const KljJniProgramHeader *,size_t phnum,jclass defining_class,KljJniImageABI,void *owner);
/* Rollback before original constructors have started. This is not dlclose or
 * a concurrent-unload API; normal native-library ownership remains VM lifetime. */
void klj_jni_image_abort(KljJniImage *);
int klj_jni_provider_address(const void *);
const void *klj_jni_provider_identity(const void *target);
void *klj_jni_provider_native_abi(void *,const char *,char *,size_t);
int klj_registration_prepare_facade(JNIEnv *);
int klj_class_lookup_bind_range(JNIEnv *,uintptr_t,size_t,jclass,const void *,int(*)(const void *));
