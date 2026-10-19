#define KLJ_BINDER_DARWIN_IMPLEMENTATION
#include "darwin.h"
#undef socket
#undef socketpair
#undef accept4
#include "OS.h"
#include "Utils.h"
#include <binder/RpcTransportRaw.h>
#include <pthread.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <sys/uio.h>

static int configured(int fd,int flags){
    if(fd<0)return fd;
    if(((flags&SOCK_CLOEXEC)&&fcntl(fd,F_SETFD,FD_CLOEXEC)<0)||((flags&SOCK_NONBLOCK)&&fcntl(fd,F_SETFL,O_NONBLOCK)<0)){
        int saved=errno;close(fd);errno=saved;return -1;
    }
    return fd;
}
extern "C" int klj_binder_socket(int domain,int type,int protocol){return configured(socket(domain,type&~(SOCK_CLOEXEC|SOCK_NONBLOCK),protocol),type);}
extern "C" int klj_binder_accept4(int fd,sockaddr* address,socklen_t* size,int flags){if(flags&~(SOCK_CLOEXEC|SOCK_NONBLOCK)){errno=EINVAL;return -1;}return configured(accept(fd,address,size),flags);}
extern "C" int klj_binder_socketpair(int domain,int type,int protocol,int* sockets){if(socketpair(domain,type&~(SOCK_CLOEXEC|SOCK_NONBLOCK),protocol,sockets)<0)return -1;if(configured(sockets[0],type)<0){int saved=errno;close(sockets[1]);errno=saved;return -1;}if(configured(sockets[1],type)<0){int saved=errno;close(sockets[0]);errno=saved;return -1;}return 0;}

namespace android::binder::os {
void trace_begin(uint64_t,const char*){}
void trace_end(uint64_t){}
void trace_int(uint64_t,const char*,int32_t){}
uint64_t get_trace_enabled_tags(){return 0;}
uint64_t GetThreadId(){uint64_t value=0;pthread_threadid_np(nullptr,&value);return value;}
bool report_sysprop_change(){return false;}
status_t setNonBlocking(borrowed_fd fd){int value=fcntl(fd.get(),F_GETFL);return value<0||fcntl(fd.get(),F_SETFL,value|O_NONBLOCK)<0?-errno:OK;}
status_t getRandomBytes(uint8_t* data,size_t size){arc4random_buf(data,size);return OK;}
status_t dupFileDescriptor(int source,int* result){int fd=fcntl(source,F_DUPFD_CLOEXEC,0);if(fd<0)return -errno;*result=fd;return OK;}
std::unique_ptr<RpcTransportCtxFactory> makeDefaultRpcTransportCtxFactory(){return RpcTransportCtxFactoryRaw::make();}
ssize_t sendMessageOnSocket(const RpcTransportFd& socket,iovec* iovs,int niovs,const std::vector<std::variant<unique_fd,borrowed_fd>>* fds){
    std::vector<int> values;if(fds){values.reserve(fds->size());for(const auto& fd:*fds)values.push_back(std::visit([](const auto& value){return value.get();},fd));}
    std::vector<char> ancillary(values.empty()?0:CMSG_SPACE(sizeof(int)*values.size()));
    msghdr message{};message.msg_iov=iovs;message.msg_iovlen=niovs;
    if(!ancillary.empty()){message.msg_control=ancillary.data();message.msg_controllen=ancillary.size();cmsghdr* control=CMSG_FIRSTHDR(&message);control->cmsg_level=SOL_SOCKET;control->cmsg_type=SCM_RIGHTS;control->cmsg_len=CMSG_LEN(sizeof(int)*values.size());memcpy(CMSG_DATA(control),values.data(),sizeof(int)*values.size());}
    return TEMP_FAILURE_RETRY(sendmsg(socket.fd.get(),&message,MSG_NOSIGNAL));
}
ssize_t receiveMessageFromSocket(const RpcTransportFd& socket,iovec* iovs,int niovs,std::vector<std::variant<unique_fd,borrowed_fd>>* fds){
    // Match the complete Unix FD envelope supported by upstream RpcState.
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int)*253)];
    msghdr message{};message.msg_iov=iovs;message.msg_iovlen=niovs;
    if(fds){message.msg_control=ancillary;message.msg_controllen=sizeof(ancillary);}
    ssize_t size=TEMP_FAILURE_RETRY(recvmsg(socket.fd.get(),&message,0));if(size<0)return size;
    std::vector<unique_fd> received;
    if(fds)for(cmsghdr* control=CMSG_FIRSTHDR(&message);control;control=CMSG_NXTHDR(&message,control))if(control->cmsg_level==SOL_SOCKET&&control->cmsg_type==SCM_RIGHTS){
        if(control->cmsg_len<CMSG_LEN(0)){errno=EPROTO;return -1;}
        size_t bytes=control->cmsg_len-CMSG_LEN(0);if(bytes%sizeof(int)){errno=EPROTO;return -1;}
        for(size_t offset=0;offset<bytes;offset+=sizeof(int)){int fd;memcpy(&fd,CMSG_DATA(control)+offset,sizeof(fd));received.emplace_back(fd);}
    }
    if(message.msg_flags&MSG_CTRUNC){errno=EMSGSIZE;return -1;}
    for(auto& fd:received)if(fcntl(fd.get(),F_SETFD,FD_CLOEXEC)<0)return -1;
    if(fds)for(auto& fd:received)fds->emplace_back(std::move(fd));return size;
}
}
LIBBINDER_EXPORTED int __android_log_print(int,const char*,const char* format,...){va_list args;va_start(args,format);int count=vfprintf(stderr,format,args);va_end(args);return count;}
// Darwin has no Android event-log device. Preserve the security event fields
// through the same actual log sink as Binder diagnostics, including binary data.
extern "C" LIBBINDER_EXPORTED int __android_log_error_write(int tag,const char* subTag,int32_t uid,const char* data,uint32_t size){
    if(!subTag||(!data&&size))return -EINVAL;
    try {
        constexpr char hex[]="0123456789abcdef";
        std::string payload;payload.resize(static_cast<size_t>(size)*2);
        for(size_t i=0;i<size;++i){const auto value=static_cast<unsigned char>(data[i]);payload[i*2]=hex[value>>4];payload[i*2+1]=hex[value&15];}
        return __android_log_print(ANDROID_LOG_ERROR,"security","security: tag=%x subtag=%s uid=%d data=%s\n",tag,subTag,uid,payload.c_str());
    } catch(const std::bad_alloc&) {return -ENOMEM;}
}
