#pragma once
#include <sys/types.h>
#include <sys/uio.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Native iovec representation. Both VM bindings project their guest addresses
 * before entering this fault-safe physical operation. IDs remain Android IDs. */
ssize_t klj_process_vm_readv(pid_t,const struct iovec *,unsigned long,const struct iovec *,unsigned long,unsigned long);
ssize_t klj_process_vm_writev(pid_t,const struct iovec *,unsigned long,const struct iovec *,unsigned long,unsigned long);
#ifdef __cplusplus
}
#endif
