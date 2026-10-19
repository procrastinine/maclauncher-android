#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Physical ABI of the shared image registry's ARM exception-index query. */
uintptr_t dl_unwind_find_exidx(uintptr_t pc,int *count);
#ifdef __cplusplus
}
#endif
