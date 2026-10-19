#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* A representation adapter under the existing kl_image owner. Registration,
 * dependency search, publication, constructors and failure lifetime remain in
 * that loader; a format supplies only its bytes, addresses and execution ABI. */
typedef struct KlImageFormat {
    unsigned elf_class;
    void *context;
    void *(*map)(void *, const char *, uintptr_t *, size_t *, char *, size_t);
    void (*unmap)(void *);
    void *(*symbol)(void *, const char *);
    int (*initialize)(void *);
    int (*needed)(void *, const char *, char (*)[128], int, char *, size_t);
    const void *(*phdrs)(void *, unsigned *);
    int (*dependencies)(void *,const char *,int (*)(void *,const char *),void *,char *,size_t);
    void *(*map_unlinked)(void *,const char *,uintptr_t *,size_t *,char *,size_t);
    int (*relocate)(void *,void *(*)(void *,const char *),void *,char *,size_t);
    const char *(*runpath)(void *);
} KlImageFormat;
/* Physical registry metadata uses the same ELF64-width representation for
 * every CPU format. Addresses remain original guest values, not host copies. */
typedef struct KlProgramHeader {
    uint32_t type,flags;
    uint64_t offset,vaddr,paddr,filesz,memsz,align;
} KlProgramHeader;
void klj_register_image_format(const KlImageFormat *);
void *klj_image_format_payload(void *image, unsigned elf_class);
#ifdef __cplusplus
}
#endif
