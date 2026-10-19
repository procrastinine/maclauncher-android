#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Cold own-process observation of the existing private futex queue. Handles
 * identify retained pthread owners; they are not guest thread IDs or lock owners.
 * No guest word is dereferenced by the reader. A requeue changes address while
 * initialAddress and the original operation/expected value remain unchanged. */
typedef struct {
    uint64_t threadHandle,address,initialAddress,parker;
    int64_t deadlineSeconds,deadlineNanoseconds;
    uint32_t operation,expected,mask,hasTimeout,realtime;
} KljFutexWaitState;
/* Zero means a bounded snapshot. EBUSY means metadata was already locked; the
 * reader never waits. more indicates additional entries beyond capacity. */
int klj_futex_snapshot(KljFutexWaitState *states,uint32_t capacity,uint32_t *count,uint32_t *more);
#ifdef __cplusplus
}
#endif
