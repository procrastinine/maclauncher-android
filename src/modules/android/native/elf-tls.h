#pragma once
#include <stddef.h>
#include <stdint.h>

/* ELF TLS indices use module identifiers, never host addresses. Identifiers
 * are not recycled while an older physical thread can retain its DTV. */
typedef struct { uintptr_t module,offset; } KljElfTlsIndex;
/* ARM variant-I TLS follows the original eight positive Bionic TCB slots.
 * The mmap reserve is private to each retained physical thread. Unused pages
 * stay demand-zero, and offsets are never recycled after a module unload. */
#define KLJ_ELF_STATIC_TLS_START 64u
#define KLJ_ELF_STATIC_TLS_ALIGNMENT 65536u
#define KLJ_ELF_STATIC_TLS_SIZE (1024u*1024u)
uintptr_t klj_elf_tls_register(const void *initial,size_t filesz,size_t memsz,
                             size_t alignment,size_t first_byte);
void klj_elf_tls_unregister(uintptr_t module);
int klj_elf_tls_static_offset(const KljElfTlsIndex *,uintptr_t *);
/* Called by the existing physical-thread owner before publishing its TP. */
void *klj_elf_tls_thread_create(void *pointer);
void klj_elf_tls_thread_destroy(void *);
void *klj_elf_tls_address(const KljElfTlsIndex *index);
uintptr_t klj_elf_tls_descriptor(const KljElfTlsIndex *index);
void klj_elf_tls_resolver(void);
struct klj_elf_tls_descriptor_node;
