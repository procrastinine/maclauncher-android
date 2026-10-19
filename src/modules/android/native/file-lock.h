#ifndef MACLAUNCHER_ANDROID_FILE_LOCK_H
#define MACLAUNCHER_ANDROID_FILE_LOCK_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

/* Android ARM64 flock uses Linux field order and lock constants. The actual
 * lock is owned by the kernel, so native libraries and Java share contention,
 * blocking, descriptor lifetime and cross-process behavior. */
struct klj_linux_flock { int16_t type,whence; int32_t padding; int64_t start,length; int32_t pid,tail; };
_Static_assert(sizeof(struct klj_linux_flock)==32,"Android ARM64 flock size");
_Static_assert(offsetof(struct klj_linux_flock,start)==8,"Android ARM64 flock offset");
static int klj_file_lock(int fd,int command,uintptr_t argument) {
    if(!argument){errno=EFAULT;return -1;}
    struct klj_linux_flock input;memcpy(&input,(void *)argument,sizeof(input));
    short type;
    switch(input.type){case 0:type=F_RDLCK;break;case 1:type=F_WRLCK;break;case 2:type=F_UNLCK;break;default:errno=EINVAL;return -1;}
    int native;
    switch(command){
        case 5:native=F_GETLK;break;case 6:native=F_SETLK;break;case 7:native=F_SETLKW;break;
        case 36:native=F_OFD_GETLK;break;case 37:native=F_OFD_SETLK;break;case 38:native=F_OFD_SETLKW;break;
        default:errno=EINVAL;return -1;
    }
    struct flock lock={.l_start=input.start,.l_len=input.length,.l_pid=input.pid,.l_type=type,.l_whence=input.whence};
    int result=fcntl(fd,native,&lock);
    if(result==0&&(command==5||command==36)){
        input.type=lock.l_type==F_RDLCK?0:lock.l_type==F_WRLCK?1:2;
        input.whence=lock.l_whence;input.start=lock.l_start;input.length=lock.l_len;input.pid=lock.l_pid;
        memcpy((void *)argument,&input,sizeof(input));
    }
    return result;
}
#endif
