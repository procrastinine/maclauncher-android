#pragma once
#include "kl_x18.h"
#include "inline-syscall.h"
/* Kept here as well as the generated header for callers compiling against the
 * original upstream declarations and linking the adapted native owner. */
int kl_x18_emit_context(void *,size_t,uint64_t,void *,size_t,uint64_t,kl_x18_stats *,size_t *,const klx_code_context *);
int klj_x18_patch_pools_context(void *,size_t,KljInlinePool *,size_t,kl_x18_stats *,const klx_code_context *);
int klj_image_instruction_patch_context(void *,size_t,KljInlinePool *,size_t,kl_x18_stats *,const klx_code_context *);
int klj_image_instruction_emit_pools_context(void *,size_t,uintptr_t,KljInlinePool *,size_t,kl_x18_stats *,const klx_code_context *,void *const *);
