#ifndef MACLAUNCHER_NATIVE_LIBC_IMPORTS_H
#define MACLAUNCHER_NATIVE_LIBC_IMPORTS_H
// Owned LP64 Bionic entry adapters. Included after the unchanged Klepton shim
// so FILE ownership, sockaddr/msghdr conversion and errno remain shared.
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <iconv.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>
#include "poll-bridge.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <wchar.h>
#include <zlib.h>
#include "native-memory-imports.h"
#include "native-scheduling.h"

_Static_assert(sizeof(wchar_t)==4 && sizeof(size_t)==8,"Android ARM64 libc ABI");
static void *klj_memmem(const void *haystack,size_t haystack_length,const void *needle,size_t needle_length){
    // Darwin's empty-input result differs from Bionic. Actual searches remain
    // entirely in the system implementation, including binary zero bytes.
    return needle_length?memmem(haystack,haystack_length,needle,needle_length):(void*)haystack;
}
static uint32_t klj_umask(uint32_t mask) { return (uint32_t)umask((mode_t)(mask & 0777)); }
static uint32_t klj_umask_chk(uint32_t mask) {
    if(mask & ~0777u)die("umask: called with invalid mask");
    return klj_umask(mask);
}
typedef struct { kl_lx_msghdr msg_hdr; uint32_t msg_len,padding; } klj_mmsghdr;
_Static_assert(sizeof(kl_lx_msghdr)==56 && sizeof(klj_mmsghdr)==64 && offsetof(klj_mmsghdr,msg_len)==56,"Android mmsghdr ABI");

// Use the system converter, including its incremental state and errors. Bionic
// accepts punctuation-insensitive spellings of its Unicode/ASCII aliases.
static const char *klj_iconv_name(const char *name,char output[160]) {
    if(!name){errno=EINVAL;return NULL;}
    size_t length=strnlen(name,128);
    if(length==128){errno=EINVAL;return NULL;}
    const char *suffix=strstr(name,"//");
    size_t base=suffix?(size_t)(suffix-name):length,n=0;
    char normalized[128];
    for(size_t i=0;i<base;++i){unsigned char c=(unsigned char)name[i];
        if(c>='A'&&c<='Z')c+='a'-'A';
        if((c>='a'&&c<='z')||(c>='0'&&c<='9'))normalized[n++]=(char)c;
    }
    normalized[n]=0;
    const char *canonical=NULL;
    if(!strcmp(normalized,"wchart"))canonical="UTF-32LE";
    else if(!strcmp(normalized,"utf8"))canonical="UTF-8";
    else if(!strcmp(normalized,"utf16le"))canonical="UTF-16LE";
    else if(!strcmp(normalized,"utf16be"))canonical="UTF-16BE";
    else if(!strcmp(normalized,"utf32le"))canonical="UTF-32LE";
    else if(!strcmp(normalized,"utf32be"))canonical="UTF-32BE";
    else if(!strcmp(normalized,"ascii")||!strcmp(normalized,"usascii"))canonical="ASCII";
    if(!canonical)return name;
    snprintf(output,160,"%s%s",canonical,suffix?suffix:"");return output;
}
static iconv_t klj_iconv_open(const char *to,const char *from) {
    char a[160],b[160];const char *target=klj_iconv_name(to,a),*source=klj_iconv_name(from,b);
    return target&&source?iconv_open(target,source):(iconv_t)-1;
}
static size_t klj_iconv(iconv_t converter,char **input,size_t *remaining,char **output,size_t *capacity) {
    if(converter==(iconv_t)-1){errno=EBADF;return (size_t)-1;}
    return iconv(converter,input,remaining,output,capacity);
}
static int klj_iconv_close(iconv_t converter) {
    if(converter==(iconv_t)-1){errno=EBADF;return -1;}
    return iconv_close(converter);
}
static void *klj_memchr_chk(const void *buffer,int value,size_t count,size_t capacity) {
    if(count>capacity)die("__memchr_chk source buffer overflow");
    return memchr(buffer,value,count);
}
// tmpnam's returned name is unreserved, as on Android, but belongs to the
// application's private temporary directory instead of the host's global /tmp.
static char *klj_tmpnam(char *result) {
    static _Thread_local char temporary[1024];
    const char *directory=getenv("TMPDIR");
    if(!directory||!*directory){errno=ENOENT;return NULL;}
    int length=snprintf(temporary,sizeof temporary,"%s/kl-XXXXXX",directory);
    if(length<0||(size_t)length>=sizeof temporary){errno=ENAMETOOLONG;return NULL;}
    int fd=mkstemp(temporary);if(fd<0)return NULL;
    int failure=close(fd);int saved=errno;
    if(unlink(temporary)<0)return NULL;
    if(failure<0){errno=saved;return NULL;}
    if(result){memcpy(result,temporary,(size_t)length+1);return result;}
    return temporary;
}
// Bionic LP64 fpos_t is a signed 64-bit stream position. Preserve Klepton's
// FILE translation, including its OBB-backed streams and standard handles.
static int klj_fgetpos(void *stream,int64_t *position){
    if(!position){errno=EINVAL;return -1;}
    off_t result=kl_ftello(stream);if(result<0)return -1;
    *position=(int64_t)result;return 0;
}
static int klj_fsetpos(void *stream,const int64_t *position){
    if(!position){errno=EINVAL;return -1;}
    return kl_fseeko(stream,(off_t)*position,SEEK_SET);
}
static size_t klj_fread_chk(void *buffer,size_t size,size_t count,void *stream,size_t capacity) {
    if(size&&count>SIZE_MAX/size){errno=EOVERFLOW;return 0;}
    if(size*count>capacity)die("__fread_chk destination buffer overflow");
    return kl_fread(buffer,size,count,stream);
}
static int klj_send_flags(int fd,int flags) {
    // Linux OOB/DONTROUTE/DONTWAIT/EOR/NOSIGNAL. Do not silently discard
    // MSG_MORE, FASTOPEN, ZEROCOPY or other Linux-only delivery requests.
    if((unsigned)flags&~(1u|4u|0x40u|0x80u|0x4000u)){errno=ENOTSUP;return 0;}
    if(flags&0x40){int status=fcntl(fd,F_GETFL);if(status<0)return 0;
        // Darwin sendmsg may block despite MSG_DONTWAIT. Never toggle a
        // descriptor shared with another caller to approximate this request.
        if(!(status&O_NONBLOCK)){errno=ENOTSUP;return 0;}
    }
    return 1;
}
static ssize_t klj_sendto_chk(int fd,const void *buffer,size_t length,size_t capacity,int flags,const struct sockaddr *address,socklen_t address_length) {
    if(length>capacity)die("__sendto_chk source buffer overflow");
    if(length>SSIZE_MAX)die("__sendto_chk count exceeds SSIZE_MAX");
    if(!klj_send_flags(fd,flags))return -1;
    return kl_sendto(fd,buffer,length,flags,address,address_length);
}
static int klj_poll_chk(struct pollfd *fds,unsigned count,int timeout,size_t capacity) {
    if(count>capacity/sizeof(*fds))die("__poll_chk descriptor array overflow");
    return klj_poll(fds,count,timeout);
}
static int klj_socket_type(int fd) {
    int type;socklen_t length=sizeof(type);
    return getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length)?-1:type;
}
static int klj_message_valid(const kl_lx_msghdr *message) {
    if(message->msg_iovlen>1024){errno=EMSGSIZE;return 0;}
    if(message->msg_iovlen&&!message->msg_iov){errno=EFAULT;return 0;}
    if(message->msg_controllen&&!message->msg_control){errno=EFAULT;return 0;}
    return 1;
}
static int klj_sendmmsg(int fd,klj_mmsghdr *messages,unsigned count,int flags) {
    if(klj_socket_type(fd)<0)return -1;
    if(!count)return 0;
    if(!messages){errno=EFAULT;return -1;}
    if(!klj_send_flags(fd,flags))return -1;
    if(count>1024)count=1024; // Linux UIO_MAXIOV sendmmsg cap.
    unsigned sent=0;
    for(;sent<count;++sent){
        kl_lx_msghdr *message=&messages[sent].msg_hdr;
        if(!klj_message_valid(message))break;
        size_t length=0;
        for(size_t i=0;i<message->msg_iovlen;++i){
            if(message->msg_iov[i].iov_len>SSIZE_MAX-length){errno=EMSGSIZE;goto finished;}
            length+=message->msg_iov[i].iov_len;
        }
        ssize_t result=kl_sendmsg(fd,message,flags);
        if(result<0)break;
        messages[sent].msg_len=(uint32_t)result;
        if((size_t)result<length){++sent;break;}
    }
finished:return sent?(int)sent:-1;
}
static int klj_recvmmsg(int fd,klj_mmsghdr *messages,unsigned count,int flags,struct timespec *timeout) {
    if(timeout){errno=timeout->tv_sec>=0&&timeout->tv_nsec>=0&&timeout->tv_nsec<1000000000?ENOSYS:EINVAL;return -1;}
    // OOB/PEEK/DONTWAIT/WAITALL/WAITFORONE only. In particular Linux's input
    // MSG_TRUNC return-length rule is not Darwin's contract. The shared
    // ancillary converter applies Linux CMSG_CLOEXEC to received descriptors.
    if((unsigned)flags&~(1u|2u|0x40u|0x100u|0x10000u|0x40000000u)){errno=ENOTSUP;return -1;}
    if(klj_socket_type(fd)<0)return -1;
    if(!count)return 0;
    if(!messages){errno=EFAULT;return -1;}
    if(count>1024){errno=EINVAL;return -1;}
    unsigned received=0;
    for(;received<count;++received){
        kl_lx_msghdr *message=&messages[received].msg_hdr;
        if(!klj_message_valid(message))break;
        ssize_t result=kl_recvmsg(fd,message,flags&~0x10000);
        if(result<0)break;
        messages[received].msg_len=(uint32_t)result;
        if(flags&0x10000)flags|=0x40;
        if(message->msg_flags&1){++received;break;}
    }
    return received?(int)received:-1;
}
static void *klj_native_libc_lookup(const char *name) {
#define KLJ_LIBC(symbol,implementation) if(!strcmp(name,symbol))return (void *)implementation
    // These are data exports, kept with the native tzset/localtime owner.
    KLJ_LIBC("timezone",&timezone);KLJ_LIBC("daylight",&daylight);KLJ_LIBC("tzname",tzname);
    KLJ_LIBC("getopt",getopt);KLJ_LIBC("getopt_long",getopt_long);KLJ_LIBC("getopt_long_only",getopt_long_only);
    KLJ_LIBC("optarg",&optarg);KLJ_LIBC("optind",&optind);KLJ_LIBC("opterr",&opterr);KLJ_LIBC("optopt",&optopt);KLJ_LIBC("optreset",&optreset);
    KLJ_LIBC("_Exit",_Exit);
    KLJ_LIBC("sched_getcpu",klj_sched_getcpu);KLJ_LIBC("getcpu",klj_getcpu);
    KLJ_LIBC("umask",klj_umask);KLJ_LIBC("__umask_chk",klj_umask_chk);
    KLJ_LIBC("mremap",klj_mremap);
    KLJ_LIBC("msync",klj_msync);KLJ_LIBC("mincore",klj_mincore);
    KLJ_LIBC("posix_madvise",klj_posix_madvise);
    // These ARM64 LP64 interfaces have the same value ABI on both platforms.
    KLJ_LIBC("memmem",klj_memmem);
    KLJ_LIBC("strndup",strndup);KLJ_LIBC("arc4random_uniform",arc4random_uniform);
    KLJ_LIBC("abs",abs);KLJ_LIBC("labs",labs);KLJ_LIBC("llabs",llabs);KLJ_LIBC("imaxabs",imaxabs);
    KLJ_LIBC("div",div);KLJ_LIBC("ldiv",ldiv);KLJ_LIBC("lldiv",lldiv);KLJ_LIBC("imaxdiv",imaxdiv);
    KLJ_LIBC("strtoimax",strtoimax);KLJ_LIBC("strtoumax",strtoumax);
    KLJ_LIBC("wcstoimax",wcstoimax);KLJ_LIBC("wcstoumax",wcstoumax);
    KLJ_LIBC("zError",zError);
    KLJ_LIBC("fgetpos",klj_fgetpos);KLJ_LIBC("fgetpos64",klj_fgetpos);KLJ_LIBC("fsetpos",klj_fsetpos);KLJ_LIBC("fsetpos64",klj_fsetpos);
    KLJ_LIBC("tmpnam",klj_tmpnam);
    KLJ_LIBC("iconv_open",klj_iconv_open);KLJ_LIBC("iconv_close",klj_iconv_close);KLJ_LIBC("iconv",klj_iconv);
    KLJ_LIBC("wcscasecmp",wcscasecmp);KLJ_LIBC("wcsncasecmp",wcsncasecmp);
    KLJ_LIBC("__sendto_chk",klj_sendto_chk);KLJ_LIBC("__fread_chk",klj_fread_chk);
    KLJ_LIBC("__poll_chk",klj_poll_chk);KLJ_LIBC("__memchr_chk",klj_memchr_chk);
    KLJ_LIBC("sendmmsg",klj_sendmmsg);KLJ_LIBC("recvmmsg",klj_recvmmsg);
#undef KLJ_LIBC
    return NULL;
}
#endif
