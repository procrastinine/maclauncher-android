#pragma once
#include <stddef.h>
#include "instruction-context.h"
/* Counts/patches the Linux syscall and original AIM TP instruction bodies. */
size_t klj_inline_syscall_count(const void *code,size_t size);
int klj_inline_syscall_patch(void *code,size_t size,void *arena,size_t arena_size,size_t *arena_used);
size_t klj_inline_syscall_count_context(const void *code,size_t size,const klx_code_context *context);
int klj_inline_syscall_patch_context(void *code,size_t size,void *arena,size_t arena_size,size_t *arena_used,const klx_code_context *context);
int klj_inline_syscall_emit_context(void *code,size_t size,uintptr_t address,void *arena,size_t arena_size,size_t *arena_used,const klx_code_context *context);
