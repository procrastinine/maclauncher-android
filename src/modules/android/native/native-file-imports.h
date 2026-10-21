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
#include <sys/mount.h>
#include <sys/socket.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
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
static int klj_zero_file_range(int fd,off_t start,off_t end){
    static const char zeros[65536]={0};
    while(start<end){size_t bytes=(uint64_t)(end-start)>sizeof(zeros)?sizeof(zeros):(size_t)(end-start);
        ssize_t written=pwrite(fd,zeros,bytes,start);
        if(written<0){if(errno==EINTR)continue;return -1;}
        if(!written){errno=EIO;return -1;}start+=written;
    }
    return 0;
}
/* Darwin punches whole filesystem blocks. Linux additionally zeroes partial
 * edge blocks, without extending EOF. Discover native support before touching
 * either edge so an unsupported punch cannot become a successful zero write. */
static int klj_punch_file_range(int fd,off_t offset,off_t length,off_t size){
    struct statfs filesystem;if(fstatfs(fd,&filesystem))return -1;
    uint64_t block=(uint64_t)filesystem.f_bsize;
    if(!block||block>LLONG_MAX){errno=EOPNOTSUPP;return -1;}
    off_t end=offset+length;if(end>size)end=size;
    off_t first=offset,last=offset;
    if(end>offset){
        off_t remainder=offset%(off_t)block;
        off_t prefix=remainder?(off_t)block-remainder:0;
        if(prefix<=end-offset){first=offset+prefix;last=end-end%(off_t)block;}
    }
    int whole=last>first;
    fpunchhole_t punch={.fp_offset=whole?first:offset-offset%(off_t)block,
        .fp_length=whole?last-first:0};
    if(fcntl(fd,F_PUNCHHOLE,&punch))return -1;
    if(!whole)return klj_zero_file_range(fd,offset,end);
    if(klj_zero_file_range(fd,offset,first))return -1;
    return klj_zero_file_range(fd,last,end);
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
    if(mode&PUNCH){result=klj_punch_file_range(fd,offset,length,info.st_size);goto done;}
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
        if(klj_zero_file_range(fd,offset,end))goto done;
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
/* Both ARM64 names use a 64-bit offset. Keep descriptions alive across the
 * host operations and advance the source only by bytes the destination took.
 * Socket output uses Darwin's actual zero-copy transfer; ordinary output keeps
 * Linux's regular-file/pipe support through bounded positioned reads. */
static ssize_t klj_sendfile_retained(int output,int input,off_t *offset,size_t count){
    struct stat source,destination;
    if(fstat(input,&source)||fstat(output,&destination))return -1;
    int in_flags=fcntl(input,F_GETFL),out_flags=fcntl(output,F_GETFL);
    if(in_flags<0||out_flags<0)return -1;
    if((in_flags&O_ACCMODE)==O_WRONLY||(out_flags&O_ACCMODE)==O_RDONLY){errno=EBADF;return -1;}
    if(out_flags&O_APPEND){errno=EINVAL;return -1;}
    if(!S_ISREG(source.st_mode)){
        if(offset&&lseek(input,0,SEEK_CUR)<0&&errno==ESPIPE)return -1;
        errno=EINVAL;return -1;
    }
    off_t position=offset?*offset:lseek(input,0,SEEK_CUR);
    if(position<0){if(offset)errno=EINVAL;return -1;}
    if(count>0x7ffff000u)count=0x7ffff000u;
    if(count>(uint64_t)(INT64_MAX-position)){errno=EOVERFLOW;return -1;}
    if(!count)return 0;
    int saved=errno,error=0;size_t done=0;
    if(S_ISSOCK(destination.st_mode)){
        off_t transferred=(off_t)count;
        if(sendfile(input,output,position,&transferred,NULL,0)<0)error=errno;
        if(transferred<0||(uint64_t)transferred>count){errno=EIO;return -1;}
        done=(size_t)transferred;
    }else{
        char buffer[65536];
        while(done<count){
            size_t wanted=count-done;if(wanted>sizeof buffer)wanted=sizeof buffer;
            ssize_t got=pread(input,buffer,wanted,position+(off_t)done);
            if(got<0){error=errno;break;}if(!got)break;
            ssize_t sent=klj_pollable_write(output,buffer,(size_t)got);
            if(sent<0){error=errno;break;}
            if((size_t)sent>(size_t)got){error=EIO;break;}
            done+=(size_t)sent;if(sent<got)break;
        }
    }
    if(done){
        if(offset)*offset=position+(off_t)done;
        else if(lseek(input,(off_t)done,SEEK_CUR)<0)return -1;
        errno=saved;return (ssize_t)done;
    }
    if(error){errno=error;return -1;}errno=saved;return 0;
}
/* Use the same own-task fault-contained copy as process_vm. Validate both
 * permissions before I/O; a stale mapping at final copyout remains EFAULT. */
static int klj_sendfile_offset(off_t *guest,off_t *value,int write_value){
    uintptr_t start=(uintptr_t)guest;if(start>UINTPTR_MAX-sizeof(*value)){errno=EFAULT;return -1;}
    uintptr_t end=start+sizeof(*value),cursor=start;
    while(cursor<end){
        mach_vm_address_t base=cursor;mach_vm_size_t length=0;natural_t depth=0;
        vm_region_submap_info_data_64_t info;mach_msg_type_number_t fields;
        kern_return_t status;
        do{fields=VM_REGION_SUBMAP_INFO_COUNT_64;status=mach_vm_region_recurse(mach_task_self(),&base,&length,&depth,(vm_region_recurse_info_t)&info,&fields);if(status!=KERN_SUCCESS)break;if(info.is_submap)depth++;}while(info.is_submap);
        if(status!=KERN_SUCCESS||base>cursor||length<=cursor-base||length>UINT64_MAX-base||
           (info.protection&(VM_PROT_READ|VM_PROT_WRITE))!=(VM_PROT_READ|VM_PROT_WRITE)){errno=EFAULT;return -1;}
        cursor=base+length;
    }
    mach_vm_size_t copied=0;
    mach_vm_address_t source=write_value?(uintptr_t)value:start,destination=write_value?start:(uintptr_t)value;
    if(mach_vm_read_overwrite(mach_task_self(),source,sizeof(*value),destination,&copied)!=KERN_SUCCESS||copied!=sizeof(*value)){errno=EFAULT;return -1;}
    if(!write_value){
        // Writing the unchanged value also validates the actual copyout path
        // before a transfer can consume input or produce output.
        copied=0;if(mach_vm_read_overwrite(mach_task_self(),(uintptr_t)value,sizeof(*value),start,&copied)!=KERN_SUCCESS||copied!=sizeof(*value)){errno=EFAULT;return -1;}
    }
    return 0;
}
static ssize_t klj_sendfile(int output,int input,off_t *offset,size_t count){
    off_t position=0;if(offset&&klj_sendfile_offset(offset,&position,0))return -1;
    if(kl_obbmap_is_vfd(input)||kl_obbmap_is_vfd(output)){errno=EINVAL;return -1;}
    if((klj_pollable_owned(input)&&!klj_pollable_physical(input))||
       (klj_pollable_owned(output)&&!klj_pollable_physical(output))){errno=EINVAL;return -1;}
    int cancellation;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancellation);
    int in=klj_pollable_fcntl(input,F_DUPFD_CLOEXEC,0),out=-1;
    if(in>=0)out=klj_pollable_fcntl(output,F_DUPFD_CLOEXEC,0);
    ssize_t result=out<0?-1:klj_sendfile_retained(out,in,offset?&position:NULL,count);int error=errno;
    if(result>=0&&offset&&klj_sendfile_offset(offset,&position,1)){result=-1;error=EFAULT;}
    if(out>=0)klj_pollable_close(out);if(in>=0)klj_pollable_close(in);
    pthread_setcancelstate(cancellation,NULL);errno=error;return result;
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
    if(!strcmp(name,"sendfile")||!strcmp(name,"sendfile64"))return (void *)klj_sendfile;
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
