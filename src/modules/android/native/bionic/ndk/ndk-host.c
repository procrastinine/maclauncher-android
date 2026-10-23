#include "ndk-host.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
extern int kl_errno_to_linux(int);
int klj_bionic_ndk_send_descriptor(int socket,int descriptor){
 char byte=0,control[CMSG_SPACE(sizeof(int))];
 struct iovec io={&byte,1};struct msghdr message={0};
 memset(control,0,sizeof(control));message.msg_iov=&io;message.msg_iovlen=1;
 message.msg_control=control;message.msg_controllen=sizeof(control);
 struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;
 header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
 memcpy(CMSG_DATA(header),&descriptor,sizeof(descriptor));
 ssize_t sent;do{sent=sendmsg(socket,&message,0);}while(sent<0&&errno==EINTR);
 return sent==1?0:kl_errno_to_linux(sent<0?errno:EIO);
}
