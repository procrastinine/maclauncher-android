#define EPOLL_SHIM_DISABLE_WRAPPER_MACROS 1
#include "bridge.h"
#include "../signal-bridge.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
extern int epoll_shim_close(int);
extern ssize_t epoll_shim_read(int,void *,size_t);
extern ssize_t epoll_shim_write(int,const void *,size_t);
extern int epoll_shim_poll(struct pollfd *,nfds_t,int);
extern int epoll_shim_ppoll(struct pollfd *,nfds_t,const struct timespec *,const sigset_t *);
extern int epoll_shim_fcntl(int,int,...);
extern int klj_pollable_duplicate(int,int,int);
extern size_t klj_pollable_vector_limit(int,int);
extern int klj_pollable_physical(int);
static int flags(int value,int extra){
    if(value&~(0x80000|0x800|extra)){errno=EINVAL;return -1;}
    return ((value&0x80000)?O_CLOEXEC:0)|((value&0x800)?O_NONBLOCK:0)|(value&extra);
}
static int guest_eventfd(unsigned initial,int mode){int native=flags(mode,1);return native<0?-1:eventfd(initial,native);}
static int guest_epoll_create1(int mode){if(mode&~0x80000){errno=EINVAL;return -1;}return epoll_create1(mode?EPOLL_CLOEXEC:0);}
static int guest_epoll_ctl(int owner,int operation,int fd,struct epoll_event *event){
    /* EPOLLWAKEUP requires Linux CAP_BLOCK_SUSPEND. Without that capability
     * Linux ignores the hint, rather than rejecting the descriptor. The host
     * power owner separately holds real macOS assertions for delivered work. */
    if(!event||operation==EPOLL_CTL_DEL)return epoll_ctl(owner,operation,fd,event);
    struct epoll_event native=*event;native.events&=~EPOLLWAKEUP;
    return epoll_ctl(owner,operation,fd,&native);
}
static int guest_epoll_pwait(int fd,struct epoll_event *events,int count,int timeout,const uint64_t *mask){sigset_t host=mask?klj_signal_mask_to_host(*mask):0;return epoll_pwait(fd,events,count,timeout,mask?&host:NULL);}
static int guest_signalfd(int fd,const uint64_t *mask,int mode){if(!mask){errno=EFAULT;return -1;}int native=flags(mode,0);if(native<0)return -1;sigset_t host=klj_signal_mask_to_host(*mask);return signalfd(fd,&host,native);}
static int guest_timerfd_create(int clock,int mode){int native=flags(mode,0);if(native<0)return -1;int actual;
    switch(clock){case 0:case 8:actual=CLOCK_REALTIME;break;case 1:actual=CLOCK_UPTIME_RAW;break;case 7:case 9:actual=CLOCK_MONOTONIC_RAW;break;default:errno=EINVAL;return -1;}
    return timerfd_create(actual,native);
}
int klj_pollable_close(int fd){return epoll_shim_close(fd);}
ssize_t klj_pollable_read(int fd,void *buffer,size_t count){return epoll_shim_read(fd,buffer,count);}
ssize_t klj_pollable_write(int fd,const void *buffer,size_t count){return epoll_shim_write(fd,buffer,count);}
/* Kernel descriptor backing storage is private. Ordinary positioned file I/O
 * must not expose or mutate counter/timer or directory cursor metadata. A future mmap-capable
 * device supplies its own exact mapping operation through this same owner. */
ssize_t klj_pollable_pread(int fd,void *buffer,size_t count,off_t offset){if(klj_pollable_owned(fd)&&!klj_pollable_physical(fd)){errno=ESPIPE;return -1;}return pread(fd,buffer,count,offset);}
ssize_t klj_pollable_pwrite(int fd,const void *buffer,size_t count,off_t offset){if(klj_pollable_owned(fd)&&!klj_pollable_physical(fd)){errno=ESPIPE;return -1;}return pwrite(fd,buffer,count,offset);}
int klj_pollable_ftruncate(int fd,off_t size){if(klj_pollable_owned(fd)&&!klj_pollable_physical(fd)){errno=EINVAL;return -1;}return ftruncate(fd,size);}
void *klj_pollable_mmap(void *address,size_t length,int protection,int flags,int fd,off_t offset){
    if(!(flags&MAP_ANON)&&klj_pollable_owned(fd)&&!klj_pollable_physical(fd)){errno=ENODEV;return MAP_FAILED;}
    return mmap(address,length,protection,flags,fd,offset);
}
int klj_pollable_poll(struct pollfd *fds,nfds_t count,int milliseconds){return epoll_shim_poll(fds,count,milliseconds);}
int klj_pollable_ppoll(struct pollfd *fds,nfds_t count,const struct timespec *timeout,const sigset_t *mask){return epoll_shim_ppoll(fds,count,timeout,mask);}
int klj_pollable_pselect(int count,fd_set *readable,fd_set *writable,fd_set *exceptional,const struct timespec *timeout,const sigset_t *mask){
    if(count<0||count>FD_SETSIZE||(timeout&&(timeout->tv_sec<0||timeout->tv_nsec<0||timeout->tv_nsec>=1000000000))){errno=EINVAL;return -1;}
    struct pollfd descriptors[FD_SETSIZE];nfds_t used=0;
    for(int fd=0;fd<count;fd++){
        short events=(readable&&FD_ISSET(fd,readable)?POLLIN:0)|(writable&&FD_ISSET(fd,writable)?POLLOUT:0)|(exceptional&&FD_ISSET(fd,exceptional)?POLLPRI:0);
        if(events)descriptors[used++]=(struct pollfd){fd,events,0};
    }
    if(klj_pollable_ppoll(descriptors,used,timeout,mask)<0)return -1;
    fd_set read_result,write_result,exception_result;FD_ZERO(&read_result);FD_ZERO(&write_result);FD_ZERO(&exception_result);int result=0;
    for(nfds_t i=0;i<used;i++){
        struct pollfd *value=&descriptors[i];if(value->revents&POLLNVAL){errno=EBADF;return -1;}
        if((value->events&POLLIN)&&(value->revents&(POLLIN|POLLHUP|POLLERR))){FD_SET(value->fd,&read_result);result++;}
        if((value->events&POLLOUT)&&(value->revents&(POLLOUT|POLLERR))){FD_SET(value->fd,&write_result);result++;}
        if((value->events&POLLPRI)&&(value->revents&POLLPRI)){FD_SET(value->fd,&exception_result);result++;}
    }
    if(readable)*readable=read_result;if(writable)*writable=write_result;if(exceptional)*exceptional=exception_result;return result;
}
int klj_pollable_select(int count,fd_set *readable,fd_set *writable,fd_set *exceptional,struct timeval *timeout){
    struct timespec converted;
    if(timeout){if(timeout->tv_sec<0||timeout->tv_usec<0||timeout->tv_usec>=1000000){errno=EINVAL;return -1;}converted=(struct timespec){timeout->tv_sec,(long)timeout->tv_usec*1000};}
    return klj_pollable_pselect(count,readable,writable,exceptional,timeout?&converted:NULL,NULL);
}
/* Linux LP64 fd_set has the same 1024-bit representation as Darwin. Signal
 * numbers and timeval's microsecond field do not share Darwin's ABI. Guest
 * calls therefore enter the existing descriptor wait owner after conversion;
 * native callers continue to use its native declarations above. */
_Static_assert(sizeof(fd_set)==128,"Linux descriptor-set representation changed");
static int guest_pselect(int count,fd_set *readable,fd_set *writable,fd_set *exceptional,const struct timespec *timeout,const uint64_t *mask){
    sigset_t native=mask?klj_signal_mask_to_host(*mask):0;
    return klj_pollable_pselect(count,readable,writable,exceptional,timeout,mask?&native:NULL);
}
struct guest_timeval {int64_t seconds,microseconds;};
static int guest_select(int count,fd_set *readable,fd_set *writable,fd_set *exceptional,struct guest_timeval *timeout){
    struct timespec remaining,started;
    if(timeout){
        if(timeout->seconds<0||timeout->microseconds<0||timeout->microseconds>=1000000){errno=EINVAL;return -1;}
        remaining=(struct timespec){timeout->seconds,timeout->microseconds*1000};
        if(clock_gettime(CLOCK_MONOTONIC,&started))return -1;
    }
    int result=klj_pollable_pselect(count,readable,writable,exceptional,timeout?&remaining:NULL,NULL),saved=errno;
    if(timeout){
        struct timespec ended;
        if(!clock_gettime(CLOCK_MONOTONIC,&ended)){
            int64_t seconds=ended.tv_sec-started.tv_sec,nanoseconds=ended.tv_nsec-started.tv_nsec;
            if(nanoseconds<0){--seconds;nanoseconds+=1000000000;}
            remaining.tv_sec-=seconds;remaining.tv_nsec-=nanoseconds;
            if(remaining.tv_nsec<0){--remaining.tv_sec;remaining.tv_nsec+=1000000000;}
            timeout->seconds=remaining.tv_sec<0?0:remaining.tv_sec;
            timeout->microseconds=remaining.tv_sec<0?0:remaining.tv_nsec/1000;
        }
    }
    errno=saved;return result;
}
static ssize_t vectors(int fd,const struct iovec *values,int count,int writing){
    if(!klj_pollable_owned(fd))return writing?writev(fd,values,count):readv(fd,values,count);
    size_t limit=klj_pollable_vector_limit(fd,writing);
    if(klj_pollable_physical(fd)){
        return writing?writev(fd,values,count):readv(fd,values,count);
    }
    if(count<0||count>IOV_MAX){errno=EINVAL;return -1;}if(!count)return 0;if(!values){errno=EFAULT;return -1;}
    size_t size=0;for(int i=0;i<count;i++){if(values[i].iov_len>SSIZE_MAX-size){errno=EINVAL;return -1;}size+=values[i].iov_len;}
    /* The original descriptor methods retain their length validation. Gather
     * only valid counter writes, and bound signal batches independently of a
     * caller's potentially enormous scatter buffer. Short reads are valid. */
    if(writing&&limit&&size!=limit)return epoll_shim_write(fd,NULL,size);
    if(!writing&&limit&&size>limit)size=limit;
    unsigned char local[128];void *bytes=size<=sizeof(local)?local:malloc(size);if(!bytes&&size){errno=ENOMEM;return -1;}
    size_t offset=0;if(writing)for(int i=0;i<count;i++){memcpy((char*)bytes+offset,values[i].iov_base,values[i].iov_len);offset+=values[i].iov_len;}
    ssize_t result=writing?epoll_shim_write(fd,bytes,size):epoll_shim_read(fd,bytes,size);int error=errno;
    if(!writing&&result>0){size_t left=(size_t)result;offset=0;for(int i=0;i<count&&left;i++){size_t length=values[i].iov_len<left?values[i].iov_len:left;memcpy(values[i].iov_base,(char*)bytes+offset,length);offset+=length;left-=length;}}
    if(bytes!=local)free(bytes);errno=error;return result;
}
ssize_t klj_pollable_readv(int fd,const struct iovec *values,int count){return vectors(fd,values,count,0);}
ssize_t klj_pollable_writev(int fd,const struct iovec *values,int count){return vectors(fd,values,count,1);}
int klj_pollable_fcntl(int fd,int command,...){
    uintptr_t value=0;
    if(command!=F_GETFD&&command!=F_GETFL&&command!=F_GETOWN){va_list args;va_start(args,command);value=va_arg(args,uintptr_t);va_end(args);}
    if(command==F_DUPFD||command==F_DUPFD_CLOEXEC)return klj_pollable_duplicate(fd,(int)value,command==F_DUPFD_CLOEXEC?1:0);
    return epoll_shim_fcntl(fd,command,value);
}
int klj_pollable_dup(int fd){return klj_pollable_duplicate(fd,0,0);}
int klj_pollable_dup2(int fd,int target){return klj_pollable_duplicate(fd,target,2);}
void *klj_pollable_lookup(const char *name){
#define ENTRY(n,p) if(!strcmp(name,n))return (void*)(p)
    ENTRY("eventfd",guest_eventfd);ENTRY("eventfd_read",eventfd_read);ENTRY("eventfd_write",eventfd_write);
    ENTRY("epoll_create",epoll_create);ENTRY("epoll_create1",guest_epoll_create1);ENTRY("epoll_ctl",guest_epoll_ctl);ENTRY("epoll_wait",epoll_wait);ENTRY("epoll_pwait",guest_epoll_pwait);
    ENTRY("signalfd",guest_signalfd);ENTRY("timerfd_create",guest_timerfd_create);ENTRY("timerfd_settime",timerfd_settime);ENTRY("timerfd_gettime",timerfd_gettime);
    ENTRY("select",guest_select);ENTRY("pselect",guest_pselect);ENTRY("pselect64",guest_pselect);
    ENTRY("getdents64",klj_descriptor_getdents64);ENTRY("__getdents64",klj_descriptor_getdents64);
    ENTRY("klj_descriptor_getdents64",klj_descriptor_getdents64);
#undef ENTRY
    return NULL;
}
