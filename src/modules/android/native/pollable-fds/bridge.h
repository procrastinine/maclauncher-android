#pragma once
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/select.h>
#include <signal.h>
// These declarations are shared by the core and JNI libraries. The original
// descriptor supplier compiles its private implementation with hidden visibility.
#pragma GCC visibility push(default)
#ifdef __cplusplus
extern "C" {
#endif
void *klj_pollable_lookup(const char *name);
int klj_pollable_owned(int fd);
int klj_pollable_physical(int fd);
int klj_pollable_close(int fd);
ssize_t klj_pollable_read(int fd,void *buffer,size_t count);
ssize_t klj_pollable_write(int fd,const void *buffer,size_t count);
ssize_t klj_pollable_pread(int,void *,size_t,off_t);
ssize_t klj_pollable_pwrite(int,const void *,size_t,off_t);
int klj_pollable_ftruncate(int,off_t);
void *klj_pollable_mmap(void *,size_t,int,int,int,off_t);
ssize_t klj_pollable_readv(int fd,const struct iovec *vectors,int count);
ssize_t klj_pollable_writev(int fd,const struct iovec *vectors,int count);
int klj_pollable_fcntl(int fd,int command,...);
int klj_pollable_dup(int fd);
int klj_pollable_dup2(int fd,int target);
int klj_pollable_poll(struct pollfd *fds,nfds_t count,int milliseconds);
int klj_pollable_ppoll(struct pollfd *,nfds_t,const struct timespec *,const sigset_t *);
int klj_pollable_select(int,fd_set *,fd_set *,fd_set *,struct timeval *);
int klj_pollable_pselect(int,fd_set *,fd_set *,fd_set *,const struct timespec *,const sigset_t *);
enum { KLJ_DESCRIPTOR_METADATA_MAX=4 };
typedef struct {uint32_t kind;int metadata;int extra[3];uint32_t count;void *lease;} KljDescriptorTransfer;
int klj_descriptor_export(int,KljDescriptorTransfer *);
void klj_descriptor_export_release(KljDescriptorTransfer *);
int klj_descriptor_import(int,uint32_t,int);
int klj_descriptor_importv(int,uint32_t,const int *,size_t);
int klj_descriptor_discard(int);
int klj_descriptor_release(int,int (*)(void *),void *);
int klj_descriptor_readonly_adopt(int,int);
int klj_descriptor_readonly_seal(int);
int klj_descriptor_access_adopt(int,int64_t);
int klj_directory_descriptor_adopt(int);
ssize_t klj_descriptor_getdents64(int,void *,size_t);
off_t klj_descriptor_lseek(int,off_t,int);
#ifdef __cplusplus
}
#endif
#pragma GCC visibility pop
