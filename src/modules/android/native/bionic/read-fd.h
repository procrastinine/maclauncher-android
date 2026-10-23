#ifndef KLJ_BIONIC_READ_FD_H
#define KLJ_BIONIC_READ_FD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t size,version;
    int64_t (*borrow)(int guest_fd,int *native_fd,void **lease);
    int64_t (*release)(void *lease);
} KljBionicReadFdApi;
/* ABI1: same physical admitted thread/context; status is zero or negative
 * Linux errno. Both calls preserve native errno. Borrow transfers no ownership
 * of native_fd to the consumer; retain its own duplicate before release.
 * The lease is opaque, single-release, and never a dereferenceable pointer.
 * Native-readable regular files only. FUSE, synthetic and fs-verity-managed
 * descriptions return EOPNOTSUPP; O_PATH/unreadable descriptions return EBADF.
 * No admission on a plain JVM/native thread. Output values clear on failure. */
const KljBionicReadFdApi *klj_bionic_read_fd_api_v1(void);
#ifdef __cplusplus
}
#endif
#endif
