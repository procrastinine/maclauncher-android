#pragma once
#include <stddef.h>
#include "instruction-context.h"
/* Fresh image-owned arenas. Packed arenas occupy only ELF inter-segment gaps;
 * their host pages must contain no writable original segment. */
typedef struct {void *base;size_t size,used;int packed;} KljInlinePool;
typedef struct {size_t word,pool,offset,bytes;uint32_t body[12],entry;} KljInlineSite;
/* Emit into a temporary body using its future addresses. Classification stays
 * with the original instruction owner. A rejected plan changes no guest word. */
typedef int (*KljInlineEmitter)(uint32_t,uintptr_t,uintptr_t,uint32_t *,size_t *,uint32_t *);
int klj_inline_instruction_match(uint32_t);
int klj_inline_instruction_body(uint32_t,uintptr_t,uintptr_t,uint32_t *,size_t *,uint32_t *);
int klj_inline_plan_apply(void *,uintptr_t,KljInlineSite *,size_t,KljInlinePool *,size_t,KljInlineEmitter);
int klj_inline_syscall_patch_pools_context(void *,size_t,KljInlinePool *,size_t,const klx_code_context *);
int klj_inline_syscall_pools_seal(KljInlinePool *,size_t);
/* Counts/patches the Linux syscall and original AIM TP instruction bodies. */
size_t klj_inline_syscall_count(const void *code,size_t size);
int klj_inline_syscall_patch(void *code,size_t size,void *arena,size_t arena_size,size_t *arena_used);
size_t klj_inline_syscall_count_context(const void *code,size_t size,const klx_code_context *context);
int klj_inline_syscall_patch_context(void *code,size_t size,void *arena,size_t arena_size,size_t *arena_used,const klx_code_context *context);
int klj_inline_syscall_emit_context(void *code,size_t size,uintptr_t address,void *arena,size_t arena_size,size_t *arena_used,const klx_code_context *context);
