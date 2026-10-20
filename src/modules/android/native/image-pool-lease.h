#pragma once
#include "inline-syscall.h"
/* Existing immutable ELF owners retain gap storage. A publication borrows
 * append-only unused bytes and retains its reservation until the source dies.
 * Mapping mutation is serialized by the executor's existing VM lock. */
typedef struct {
    void *image;
    size_t index,previous;
    KljInlinePool pool;
} KljImagePoolLease;
size_t klj_image_gap_pool_leases(uintptr_t,size_t,KljImagePoolLease *,size_t);
void klj_image_gap_pool_commit(KljImagePoolLease *,size_t);
int klj_image_gap_pool_release(KljImagePoolLease *,size_t);
