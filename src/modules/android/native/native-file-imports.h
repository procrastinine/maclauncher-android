#ifndef MACLAUNCHER_NATIVE_FILE_IMPORTS_H
#define MACLAUNCHER_NATIVE_FILE_IMPORTS_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include "pollable-fds/bridge.h"
#include "file-xattrs.h"
#include "proc-maps.h"
#include "process-ids.h"

extern const char *kl_guest_path(const char *,char *,size_t);
extern int kl_obbmap_is_vfd(int);
extern int kl_errno_to_linux(int);
extern int klj_fstatat(int,const char *,void *,int);

/* Darwin reserves bytes beyond physical EOF; Linux requests a file range.
 * Reuse the real descriptor and reserve only the missing capacity. A bounded
 * inode lock prevents concurrent calls from repeatedly reserving the same
 * space, without changing the application's flock/fcntl lock ownership. */
static pthread_once_t klj_allocation_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t klj_allocation_locks[32];
static void klj_allocation_before_fork(void){for(size_t i=0;i<32;i++)pthread_mutex_lock(&klj_allocation_locks[i]);}
static void klj_allocation_after_fork(void){for(size_t i=32;i>0;i--)pthread_mutex_unlock(&klj_allocation_locks[i-1]);}
static void klj_allocation_initialize(void){
    for(size_t i=0;i<32;i++)pthread_mutex_init(&klj_allocation_locks[i],NULL);
    pthread_atfork(klj_allocation_before_fork,klj_allocation_after_fork,klj_allocation_after_fork);
}
static int klj_file_allocate_retained(int fd,int mode,off_t offset,off_t length){
    enum {KEEP=1,PUNCH=2,ZERO=16};
    struct stat info;if(fstat(fd,&info))return -1;
    if(!S_ISREG(info.st_mode)){errno=S_ISDIR(info.st_mode)?EISDIR:ESPIPE;return -1;}
    int access=fcntl(fd,F_GETFL);if(access<0)return -1;
    if((access&O_ACCMODE)==O_RDONLY){errno=EBADF;return -1;}
    if((mode&(PUNCH|ZERO))&&(access&O_APPEND)){errno=EPERM;return -1;}
    pthread_once(&klj_allocation_once,klj_allocation_initialize);
    pthread_mutex_t *lock=&klj_allocation_locks[((uint64_t)info.st_dev^(uint64_t)info.st_ino)%32];
    pthread_mutex_lock(lock);
    int result=-1;
    if(fstat(fd,&info))goto done;
    if(mode&PUNCH){
        fpunchhole_t punch={.fp_offset=offset,.fp_length=length};
        result=fcntl(fd,F_PUNCHHOLE,&punch);goto done;
    }
    off_t end=offset+length;
    off_t allocated=info.st_blocks>LLONG_MAX/512?LLONG_MAX:info.st_blocks*512;
    if(end>allocated){
        fstore_t allocation={.fst_flags=F_ALLOCATEALL|F_ALLOCATEPERSIST,
            .fst_posmode=F_PEOFPOSMODE,.fst_length=end-allocated};
        if(fcntl(fd,F_PREALLOCATE,&allocation))goto done;
    }
    if(!(mode&KEEP)&&end>info.st_size&&ftruncate(fd,end))goto done;
    if(mode&ZERO){
        // Keep-size ranges may reserve capacity past EOF but must not extend
        // the visible file. pwrite preserves the caller's current offset.
        if((mode&KEEP)&&end>info.st_size)end=info.st_size;
        static const char zeros[65536]={0};
        while(offset<end){size_t bytes=(uint64_t)(end-offset)>sizeof(zeros)?sizeof(zeros):(size_t)(end-offset);
            ssize_t written=pwrite(fd,zeros,bytes,offset);
            if(written<0){if(errno==EINTR)continue;goto done;}
            if(!written){errno=EIO;goto done;}offset+=written;
        }
    }
    result=0;
done:;
    int error=errno;pthread_mutex_unlock(lock);errno=error;return result;
}
static int klj_file_allocate(int fd,int mode,off_t offset,off_t length){
    enum {KEEP=1,PUNCH=2,ZERO=16};
    if(offset<0||length<=0){errno=EINVAL;return -1;}
    if(offset>LLONG_MAX-length){errno=EFBIG;return -1;}
    if(mode&~(KEEP|PUNCH|ZERO)){errno=EOPNOTSUPP;return -1;}
    if((mode&PUNCH)&&(!(mode&KEEP)||(mode&ZERO))){errno=EINVAL;return -1;}
    if(kl_obbmap_is_vfd(fd)){errno=EBADF;return -1;}
    if(klj_pollable_owned(fd)&&!klj_pollable_physical(fd)){errno=EOPNOTSUPP;return -1;}
    // Retain the open file description across the host's multiple operations:
    // a concurrent guest close/reuse must never retarget later writes.
    int retained=fcntl(fd,F_DUPFD_CLOEXEC,0);if(retained<0)return -1;
    int result=klj_file_allocate_retained(retained,mode,offset,length),error=errno;
    close(retained);errno=error;return result;
}
static int klj_posix_fallocate(int fd,off_t offset,off_t length){
    int saved=errno,result=klj_file_allocate(fd,0,offset,length);
    int error=result?kl_errno_to_linux(errno):0;errno=saved;return error;
}
/* Android directory sentinels and flags are not Darwin values. Ordinary
 * access shares Klepton's guest namespace and virtual asset/library presence.
 * Relative descriptor checks stay in the kernel; only a missing virtual path
 * needs the descriptor's current pathname for that same existing owner. */
static int klj_faccessat(int directory,const char *name,int mode,int flags) {
    enum {LINUX_NOFOLLOW=0x100,LINUX_EACCESS=0x200};
    if((mode&~(R_OK|W_OK|X_OK))||(flags&~(LINUX_NOFOLLOW|LINUX_EACCESS))){errno=EINVAL;return -1;}
    if(!name){errno=EFAULT;return -1;}
    int fd=directory==-100?AT_FDCWD:directory;
    int (*original)(const char *,int)=kl_upstream_shim_lookup("access");
    if(!flags&&(fd==AT_FDCWD||name[0]=='/'))return original(name,mode);
    char mapped[PATH_MAX];const char *actual=kl_guest_path(name,mapped,sizeof(mapped));
    int translated=((flags&LINUX_NOFOLLOW)?AT_SYMLINK_NOFOLLOW:0)|((flags&LINUX_EACCESS)?AT_EACCESS:0);
    int result=faccessat(fd,actual,mode,translated);
    if(!result||errno!=ENOENT||!name[0])return result;
    int error=errno;
    if(fd==AT_FDCWD||actual[0]=='/'){
        if(original(actual,mode)==0)return 0;
    }else{
        char parent[PATH_MAX],joined[PATH_MAX];
        if(!fcntl(fd,F_GETPATH,parent)){
            int length=snprintf(joined,sizeof(joined),"%s/%s",parent,actual);
            if(length>0&&(size_t)length<sizeof(joined)&&original(joined,mode)==0)return 0;
        }
    }
    errno=error;return -1;
}
static ssize_t klj_readlinkat(int directory,const char *name,char *buffer,size_t length) {
    if(name&&!strncmp(name,"/proc/",6)){
        ssize_t origin=klj_proc_maps_readlink(name,buffer,length,(uint64_t)klj_process_id());
        if(origin!=-2)return origin;
    }
    char mapped[PATH_MAX];
    const char *actual=kl_guest_path(name,mapped,sizeof(mapped));
    return readlinkat(directory==-100?AT_FDCWD:directory,actual,buffer,length);
}
static ssize_t klj_readlink(const char *name,char *buffer,size_t length) {
    return klj_readlinkat(-100,name,buffer,length);
}
static void klj_readlink_bounds(size_t length,size_t capacity) {
    if(length>SSIZE_MAX||length>capacity){
        fputs("FORTIFY: readlink destination buffer overflow\n",stderr);abort();
    }
}
static ssize_t klj_readlink_chk(const char *name,char *buffer,size_t length,size_t capacity) {
    klj_readlink_bounds(length,capacity);return klj_readlink(name,buffer,length);
}
static ssize_t klj_readlinkat_chk(int directory,const char *name,char *buffer,size_t length,size_t capacity) {
    klj_readlink_bounds(length,capacity);return klj_readlinkat(directory,name,buffer,length);
}

/* Bionic openat is fixed-arity at this boundary. Keep Klepton's namespace,
 * OBB descriptors, open flags and tracing; translate only AT_FDCWD. */
static int klj_openat(int directory,const char *name,int flags,int mode) {
    if((flags&0x404000)==0x404000){errno=ENOTSUP;return -1;}
    int (*original)(int,const char *,int,int)=kl_upstream_shim_lookup("openat");
    return original(directory==-100?AT_FDCWD:directory,name,flags,mode);
}
static int klj_creat(const char *name,int mode) {
    /* Bionic O_WRONLY | O_CREAT | O_TRUNC. Keep pathname translation,
     * descriptor ownership and mode handling in the existing openat owner. */
    return klj_openat(-100,name,0x1|0x40|0x200,mode);
}
static void klj_open_require_no_mode(int flags) {
    if((flags&0x40)||(flags&0x404000)==0x404000){
        fputs("FORTIFY: open called with O_CREAT/O_TMPFILE but no mode\n",stderr);
        abort();
    }
}
static int klj_openat_2(int directory,const char *name,int flags) {
    klj_open_require_no_mode(flags);return klj_openat(directory,name,flags,0);
}
static int klj_open_2(const char *name,int flags) {
    return klj_openat_2(-100,name,flags);
}
static void *klj_native_file_lookup(const char *name) {
    void *attribute=klj_xattr_lookup(name);if(attribute)return attribute;
    if(!strcmp(name,"fstatat")||!strcmp(name,"fstatat64"))return (void *)klj_fstatat;
    if(!strcmp(name,"faccessat"))return (void *)klj_faccessat;
    if(!strcmp(name,"fallocate")||!strcmp(name,"fallocate64"))return (void *)klj_file_allocate;
    if(!strcmp(name,"posix_fallocate")||!strcmp(name,"posix_fallocate64"))return (void *)klj_posix_fallocate;
    if(!strcmp(name,"readlink"))return (void *)klj_readlink;
    if(!strcmp(name,"readlinkat"))return (void *)klj_readlinkat;
    if(!strcmp(name,"__readlink_chk"))return (void *)klj_readlink_chk;
    if(!strcmp(name,"__readlinkat_chk"))return (void *)klj_readlinkat_chk;
    if(!strcmp(name,"openat")||!strcmp(name,"openat64"))return (void *)klj_openat;
    if(!strcmp(name,"creat")||!strcmp(name,"creat64"))return (void *)klj_creat;
    if(!strcmp(name,"__openat_2"))return (void *)klj_openat_2;
    if(!strcmp(name,"__open_2"))return (void *)klj_open_2;
    return NULL;
}
#endif
