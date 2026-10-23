#pragma once
#include "cpu.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Every pointer remains leased by the memory owner through a step. */
typedef struct {
    uint64_t address, length;
    void *host;
    unsigned permissions;
    unsigned flags;
} KlA64Mapping;
typedef struct {
    void *owner;
    int (*map)(void *, uint64_t address, size_t length, unsigned access, KlA64Mapping *);
    uint64_t (*counter)(void *);
    uint32_t ctr_el0, cntfrq_el0, dczid_el0;
    void (*write)(void *, uint64_t address, size_t length);
} KlA64PreciseConfig;
enum { KL_A64_MAPPING_JIT=1, KL_A64_MAPPING_IO=2, KL_A64_MAPPING_PRIVATE_DATA=4, KL_A64_MAPPING_EXECUTION_OWNER=8 };
typedef struct KlA64Precise KlA64Precise;
KlA64Precise *kl_a64_precise_create(KlA64PreciseConfig);
void kl_a64_precise_destroy(KlA64Precise *);
/* Optional original-code cache. The callback re-acquires a complete current
   view under the caller's VM/source lease before any execution. Private RAM
   execution-owner RX pages and non-executable read/read-write IO tables qualify.
   Source byte changes invalidate original TBs; IO data stays live. Retaining an
   IO table does not grant the PRIVATE_DATA store-barrier exemption.
   Inactive cached pointers carry no access authority between calls. */
typedef int (*KlA64Revalidate)(void *,uint64_t,size_t,KlA64Mapping *);
int kl_a64_precise_set_owned_cache(KlA64Precise *,KlA64Revalidate);
typedef struct {uint64_t mapped,retired,revalidated,source_changed;} KlA64PreciseCounters;
KlA64PreciseCounters kl_a64_precise_counters(const KlA64Precise *);
/* Each step revalidates its instruction and releases all native pointer
   mappings before returning to the memory owner. Epoch identifies the lease.
   The optional owned cache is used only by run_owned with full re-admission. */
KlA64Exit kl_a64_precise_step(KlA64Precise *, KlA64State *, uint64_t mapping_epoch);
/* Retains a real host reservation only inside this call, extending an original
   exclusive interval up to its store/CLREX/exception or the bounded budget. */
KlA64Exit kl_a64_precise_run(KlA64Precise *, KlA64State *, uint64_t mapping_epoch);
/* Bounded original-code execution under one retained VM/source lease. Stops
   after a possible executable-source store, on leaving [begin,end), at SVC/exception or the budget. A real
   exclusive interval still runs through its store/CLREX or bounded refusal.
   PRIVATE_DATA is an owner-authenticated unique anonymous RW/non-X mapping;
   those stores do not publish executable-source generations. EXECUTION_OWNER
   permits continuation into another positively admitted executable view
   within the same retained mapping epoch; other range exits still return. */
KlA64Exit kl_a64_precise_run_owned(KlA64Precise *, KlA64State *, uint64_t mapping_epoch,
                                 uint64_t begin, uint64_t end, unsigned budget);
int kl_a64_precise_clear_exclusive(KlA64Precise *);
/* Resets the identical CPU/reservation state for an ordinary native boundary.
   Opted-in original RX and non-executable IO tables remain inert and
   require full re-admission. IO reads and writes always use current bytes. */
int kl_a64_precise_reset(KlA64Precise *);
uint32_t kl_a64_precise_instruction(const KlA64Precise *);
uint64_t kl_a64_precise_instruction_address(const KlA64Precise *);
/* Called only by the existing native signal owner. Real synchronous accesses
   are admitted by exact active host bounds; software signals stay untouched. */
int kl_a64_precise_native_fault(int signal, int code, uint64_t fault_address, const void *signal_mask);
#ifdef __cplusplus
}
#endif
