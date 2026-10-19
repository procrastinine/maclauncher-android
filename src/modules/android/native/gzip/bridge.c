// The original gzip implementation owns its buffers and compression state.
// Only its POSIX operations cross into the existing guest descriptor owner.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>
#include "kl_va.h"

extern void *kl_shim_lookup(const char *);
extern int klj_z_gzvprintf(gzFile,const char *,va_list);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int (*guest_openat)(int,const char *,int,int);
static ssize_t (*guest_read)(int,void *,size_t);
static ssize_t (*guest_write)(int,const void *,size_t);
static off_t (*guest_seek)(int,off_t,int);
static int (*guest_close)(int);
static void initialize(void) {
    guest_openat=kl_shim_lookup("openat");guest_read=kl_shim_lookup("read");
    guest_write=kl_shim_lookup("write");guest_seek=kl_shim_lookup("lseek");
    guest_close=kl_shim_lookup("close");
}
int klj_gzip_open(const char *name,int flags,...) {
    int mode=0;if(flags&O_CREAT){va_list arguments;va_start(arguments,flags);mode=va_arg(arguments,int);va_end(arguments);}
    int translated=flags&O_ACCMODE;
    if(flags&O_CREAT)translated|=0x40;if(flags&O_EXCL)translated|=0x80;
    if(flags&O_TRUNC)translated|=0x200;if(flags&O_APPEND)translated|=0x400;
    if(flags&O_CLOEXEC)translated|=0x80000;
    pthread_once(&once,initialize);return guest_openat(-100,name,translated,mode);
}
ssize_t klj_gzip_read(int fd,void *buffer,size_t length){pthread_once(&once,initialize);return guest_read(fd,buffer,length);}
ssize_t klj_gzip_write(int fd,const void *buffer,size_t length){pthread_once(&once,initialize);return guest_write(fd,buffer,length);}
off_t klj_gzip_seek(int fd,off_t offset,int whence){pthread_once(&once,initialize);return guest_seek(fd,offset,whence);}
int klj_gzip_close(int fd){pthread_once(&once,initialize);return guest_close(fd);}
int klj_gzip_vprintf(gzFile file,const char *format,kl_va *arguments) {
    _Alignas(16) unsigned char native[4096];
    if(kl_va_marshal(format,arguments,native,sizeof(native),KL_VA_PRINTF)==(size_t)-1)return Z_STREAM_ERROR;
    return klj_z_gzvprintf(file,format,(va_list)native);
}
