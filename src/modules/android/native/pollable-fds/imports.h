#include "bridge.h"
/* Native representations only. Guest flag/path/errno translation remains at
 * the existing libc and libcore boundaries that call these operations. */
#define write klj_pollable_write
#define pread klj_pollable_pread
#define pwrite klj_pollable_pwrite
#define ftruncate klj_pollable_ftruncate
#define mmap klj_pollable_mmap
#define read klj_pollable_read
#define readv klj_pollable_readv
#define poll klj_pollable_poll
#define ppoll klj_pollable_ppoll
#define select klj_pollable_select
#define pselect klj_pollable_pselect
#define writev klj_pollable_writev
#define fcntl klj_pollable_fcntl
#define dup klj_pollable_dup
#define dup2 klj_pollable_dup2
#define lseek klj_descriptor_lseek

