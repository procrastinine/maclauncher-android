#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU state only. The caller retains Linux, ELF, mappings and thread ownership. */
typedef struct {
    uint64_t x[31], sp, pc, v[32][2], tpidr_el0, tpidrro_el0;
    uint32_t pstate, fpcr, fpsr;
} KlA64State;

typedef struct {
    void *owner;
    int (*read)(void *, uint64_t, void *, size_t, int executable);
    int (*write)(void *, uint64_t, const void *, size_t);
    /* 1 succeeds, 0 loses the reservation, negative reports an owner error. */
    int (*exclusive)(void *, uint64_t, const void *, const void *, size_t);
    uint64_t (*counter)(void *);
} KlA64Memory;

enum {
    KL_A64_BUDGET, KL_A64_SVC, KL_A64_MEMORY, KL_A64_EXCEPTION,
    KL_A64_UNSUPPORTED, KL_A64_STOP, KL_A64_INSTRUCTION_CACHE
};
typedef struct {
    unsigned kind, detail;
    uint64_t address;
    int owner_error;
} KlA64Exit;

typedef struct KlA64Cpu KlA64Cpu;
typedef struct {
    KlA64Memory memory;
    uint32_t ctr_el0, cntfrq_el0, dczid_el0;
    int direct_memory;
} KlA64Config;
/* Direct memory uses the existing owner's actual native virtual addresses. */
KlA64Cpu *kl_a64_create(KlA64Config);
void kl_a64_destroy(KlA64Cpu *);
KlA64Exit kl_a64_run(KlA64Cpu *, KlA64State *, uint64_t instruction_budget);
void kl_a64_invalidate(KlA64Cpu *, uint64_t address, size_t length);
void kl_a64_clear_cache(KlA64Cpu *);
void kl_a64_stop(KlA64Cpu *);
/* The memory owner validates the writable range before calling this primitive. */
int kl_a64_compare_exchange(uint64_t, const void *, const void *, size_t);

#ifdef __cplusplus
}
#endif
