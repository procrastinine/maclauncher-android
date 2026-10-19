#ifndef MACLAUNCHER_NATIVE_SOCKET_IMPORTS_H
#define MACLAUNCHER_NATIVE_SOCKET_IMPORTS_H
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// Bionic includes creation flags in the socket type. Darwin requires these
// descriptor operations separately; retain Klepton's socket/address owners.
enum { KLJ_SOCK_NONBLOCK=0x800, KLJ_SOCK_CLOEXEC=0x80000 };
static int klj_socket_flags(int fd,int flags){
    if(flags&KLJ_SOCK_NONBLOCK){int value=fcntl(fd,F_GETFL);if(value<0||fcntl(fd,F_SETFL,value|O_NONBLOCK)<0)return -1;}
    if(flags&KLJ_SOCK_CLOEXEC){int value=fcntl(fd,F_GETFD);if(value<0||fcntl(fd,F_SETFD,value|FD_CLOEXEC)<0)return -1;}
    return 0;
}
int klj_socket(int domain,int type,int protocol){
    int fd=kl_socket(domain,type&~(KLJ_SOCK_NONBLOCK|KLJ_SOCK_CLOEXEC),protocol);
    if(fd>=0&&klj_socket_flags(fd,type)<0){int error=errno;close(fd);errno=error;return -1;}
    return fd;
}
int klj_socketpair(int domain,int type,int protocol,int pair[2]){
    if(domain==10)domain=AF_INET6;
    if(socketpair(domain,type&~(KLJ_SOCK_NONBLOCK|KLJ_SOCK_CLOEXEC),protocol,pair)<0)return -1;
    if(klj_socket_flags(pair[0],type)<0||klj_socket_flags(pair[1],type)<0){
        int error=errno;close(pair[0]);close(pair[1]);errno=error;return -1;
    }
    return 0;
}
int klj_accept4(int fd,struct sockaddr *address,socklen_t *length,int flags){
    if(flags&~(KLJ_SOCK_NONBLOCK|KLJ_SOCK_CLOEXEC)){errno=EINVAL;return -1;}
    int accepted=kl_accept(fd,address,length);
    if(accepted>=0&&klj_socket_flags(accepted,flags)<0){int error=errno;close(accepted);errno=error;return -1;}
    return accepted;
}
// Linux's TCP_KEEPIDLE value is Darwin's TCP_NOPUSH. Passing the integer
// through silently corks writes instead of configuring idle probes.
static int klj_tcp_option(int option){
    switch(option){
        case 3:return TCP_NOPUSH;
        case 4:return TCP_KEEPALIVE;
        case 5:return TCP_KEEPINTVL;
        case 6:return TCP_KEEPCNT;
        default:return option;
    }
}
static int klj_path_mtu_option(int level,int option){
    if(level==IPPROTO_IP&&option==10)return IP_DONTFRAG;
    // Darwin exposes RFC3542's flag at62 without changing the older multicast
    // ABI selected by Klepton's headers.
    if(level==IPPROTO_IPV6&&option==23)return 62; // IPV6_DONTFRAG
    return 0;
}
/* Android LP64 timeval has two signed64 fields. Darwin's microseconds field
 * is signed32 plus padding, so even an otherwise identical socket option
 * cannot write directly into a guest timeval. */
static int klj_timeout_option(int level,int option){
    return (level==1||level==SOL_SOCKET)&&(option==20||option==21||option==66||option==67);
}
int klj_setsockopt(int fd,int level,int option,const void *value,socklen_t length){
    if(klj_timeout_option(level,option)){
        if(!value){errno=EFAULT;return -1;}
        if(length<2*sizeof(int64_t)){errno=EINVAL;return -1;}
        int64_t guest[2];memcpy(guest,value,sizeof(guest));
        if(guest[1]<0||guest[1]>=1000000){errno=EDOM;return -1;}
        struct timeval host={.tv_sec=guest[0],.tv_usec=(suseconds_t)guest[1]};
        return kl_setsockopt(fd,level,option,&host,sizeof(host));
    }
    int result,native=klj_path_mtu_option(level,option),effective=0;
    if(native){
        if(!value){errno=EFAULT;return -1;}
        if(length<sizeof(int)){errno=EINVAL;return -1;}
        int mode;memcpy(&mode,value,sizeof(mode));
        if(mode<0||mode>5){errno=EINVAL;return -1;}
        // Darwin owns path-MTU discovery. Preserve the Linux request's
        // per-socket fragmentation policy without adding shadow socket state:
        // DONT/WANT/OMIT permit fragmentation; DO/PROBE/INTERFACE forbid it.
        effective=mode>=2&&mode<=4;
        result=setsockopt(fd,level,native,&effective,sizeof(effective));
    }else if(level==IPPROTO_TCP){
        native=klj_tcp_option(option);
        result=setsockopt(fd,level,native,value,length);
    }else result=kl_setsockopt(fd,level,option,value,length);
    int error=errno;
    if(kl_net_trace()&&(native||result)){
        int requested=0;if(value&&length>=sizeof(requested))memcpy(&requested,value,sizeof(requested));
        fprintf(stderr,"  [net] setsockopt(fd=%d level=%d option=%d host=%d value=%d) -> %d errno=%d\n",fd,level,option,native?native:option,requested,result,result?kl_errno_to_linux(error):0);
    }
    errno=error;return result;
}
int klj_getsockopt(int fd,int level,int option,void *value,socklen_t *length){
    if(klj_timeout_option(level,option)){
        if(!length||(!value&&*length)){errno=EFAULT;return -1;}
        struct timeval host={0};socklen_t size=sizeof(host);
        int result=kl_getsockopt(fd,level,option,&host,&size);
        if(!result){int64_t guest[2]={host.tv_sec,host.tv_usec};size=*length<sizeof(guest)?*length:sizeof(guest);if(size)memcpy(value,guest,size);*length=size;}
        return result;
    }
    int result,native=klj_path_mtu_option(level,option);
    bool pending=(level==1||level==SOL_SOCKET)&&option==4;
    if(pending||native){
        if(!length||(!value&&*length)){errno=EFAULT;return -1;}
        int translated=0;socklen_t size=sizeof(translated);
        result=pending?kl_getsockopt(fd,level,option,&translated,&size):getsockopt(fd,level,native,&translated,&size);
        int error=errno;
        if(!result){
            translated=pending?kl_errno_to_linux(translated):(translated?2:0);
            size=*length<sizeof(translated)?*length:sizeof(translated);
            if(size)memcpy(value,&translated,size);*length=size;
            if(pending&&kl_net_trace())fprintf(stderr,"  [net] getsockopt(fd=%d SO_ERROR) -> %d\n",fd,translated);
        }
        errno=error;return result;
    }
    if(level==IPPROTO_TCP)return getsockopt(fd,level,klj_tcp_option(option),value,length);
    return kl_getsockopt(fd,level,option,value,length);
}
static void *klj_socket_lookup(const char *name){
    if(!strcmp(name,"socket"))return (void *)klj_socket;
    if(!strcmp(name,"socketpair"))return (void *)klj_socketpair;
    if(!strcmp(name,"accept4"))return (void *)klj_accept4;
    if(!strcmp(name,"setsockopt"))return (void *)klj_setsockopt;
    if(!strcmp(name,"getsockopt"))return (void *)klj_getsockopt;
    return NULL;
}
#endif
