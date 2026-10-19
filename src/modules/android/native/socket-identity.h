#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/proc_info.h>
#include <libproc.h>

/* Kernel cookies identify live open sockets; paths alone grant no transport
 * capability. The supervisor supplies the authenticated host process ID. */
static inline int klj_socket_identity(pid_t process,int fd,uint64_t expected,struct socket_info *result){
    struct socket_fdinfo value={0};
    if(process<=0||fd<0){errno=EINVAL;return -1;}
    int size=proc_pidfdinfo(process,fd,PROC_PIDFDSOCKETINFO,&value,sizeof value);
    if(size!=(int)sizeof value){if(size>=0)errno=EBADF;return -1;}
    if(value.psi.soi_family!=AF_UNIX||value.psi.soi_kind!=SOCKINFO_UN||!value.psi.soi_so){errno=EAFNOSUPPORT;return -1;}
    if(expected&&value.psi.soi_so!=expected){errno=ESTALE;return -1;}
    *result=value.psi;return 0;
}
static inline void klj_socket_address_hex(const struct sockaddr_un *address,size_t capacity,char *output){
    static const char digits[]="0123456789abcdef";
    size_t offset=offsetof(struct sockaddr_un,sun_path),size=capacity>offset?capacity-offset:0;
    if(size>sizeof address->sun_path)size=sizeof address->sun_path;
    if(address->sun_len>=offset&&address->sun_len<capacity)size=address->sun_len-offset;
    size_t length=strnlen(address->sun_path,size);
    for(size_t i=0;i<length;i++){unsigned char byte=(unsigned char)address->sun_path[i];output[i*2]=digits[byte>>4];output[i*2+1]=digits[byte&15];}
    output[length*2]=0;
}
