#pragma once
#include <stddef.h>
#include <stdint.h>

/* ELF TLS indices use module identifiers, never host addresses. Identifiers
 * are not recycled while an older physical thread can retain its DTV. */
typedef struct { uintptr_t module,offset; } KljElfTlsIndex;
uintptr_t klj_elf_tls_register(const void *initial,size_t filesz,size_t memsz,
                             size_t alignment,size_t first_byte);
void klj_elf_tls_unregister(uintptr_t module);
void *klj_elf_tls_address(const KljElfTlsIndex *index);
uintptr_t klj_elf_tls_descriptor(const KljElfTlsIndex *index);
void klj_elf_tls_resolver(void);
struct klj_elf_tls_descriptor_node;
