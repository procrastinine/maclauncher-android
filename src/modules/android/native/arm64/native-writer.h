#pragma once
#include "cpu.h"
#include <signal.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {KL_NATIVE_WRITER_SLOTS=4};
typedef struct {
 void *owner;
 int (*execute)(void *,KlA64State *,uint64_t fault_address);
 int free_slots[2],requests[2],completion[KL_NATIVE_WRITER_SLOTS][2];
} KlNativeWriterConfig;
typedef struct KlNativeWriter KlNativeWriter;
/* All descriptors are privately owned by the caller and remain valid through
 * destroy. Create/destroy run outside signal handlers; destroy requires all
 * producers to be retired. The component never installs signal dispositions. */
KlNativeWriter *kl_native_writer_create(KlNativeWriterConfig);
void kl_native_writer_destroy(KlNativeWriter *);
/* Native shared-cache submaps retain their actual leaf protections. */
int kl_native_writer_region(uint64_t,uint64_t*,uint64_t*,int*);
/* Called only after the actual memory owner admitted this hardware write fault.
 * Returns1 only after normal continuation is ready in the original frame.
 * No allocation, lock acquisition, polling or permission changes occur here.
 * Raw-fork children decline before queue admission; exec creates a new owner.
 * Native callers must respect Darwin's reserved x18 ABI. */
int kl_native_writer_fault(KlNativeWriter *,int,const siginfo_t *,void *ucontext);
#ifdef __cplusplus
}
#endif
