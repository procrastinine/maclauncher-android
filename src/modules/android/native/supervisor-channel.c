#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#define MAX_FRAME (1024 * 1024)
static int socket_ok(int fd){int type=0;socklen_t n=sizeof(type);struct sockaddr_storage address;socklen_t a=sizeof(address);return !getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&n)&&type==SOCK_STREAM&&!getsockname(fd,(struct sockaddr*)&address,&a)&&address.ss_family==AF_UNIX;}
static int ready(int fd,short events){struct pollfd p={.fd=fd,.events=events};int r;do{r=poll(&p,1,-1);}while(r<0&&errno==EINTR);return r>0?0:-1;}
static int read_all(int fd,void*bytes,size_t n){size_t at=0;while(at<n){ssize_t r=read(fd,(char*)bytes+at,n-at);if(r>0){at+=(size_t)r;continue;}if(r<0&&errno==EINTR)continue;if(r<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&!ready(fd,POLLIN))continue;if(!r)errno=EPIPE;return -1;}return 0;}

static int write_all(int fd,const void*bytes,size_t n){size_t at=0;while(at<n){ssize_t r=write(fd,(const char*)bytes+at,n-at);if(r>0){at+=(size_t)r;continue;}if(r<0&&errno==EINTR)continue;if(r<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&!ready(fd,POLLOUT))continue;if(!r)errno=EPIPE;return -1;}return 0;}
#ifdef SUPERVISOR_SENDER
#include <stdio.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/un.h>
#include <libproc.h>
static int peer_identity(void){
    pid_t pid=0;uid_t uid=0;gid_t gid=0;socklen_t size=sizeof(pid);
    if(!socket_ok(3)||getsockopt(3,SOL_LOCAL,LOCAL_PEERPID,&pid,&size)||getpeereid(3,&uid,&gid)||pid<=0){perror("Supervisor peer identity");return 1;}
    printf("{\"pid\":%d,\"uid\":%u}\n",pid,uid);return 0;
}
static int process_identity(const char *text){
    char*end=NULL;long number=strtol(text,&end,10);if(!end||*end||number<=0||number>INT32_MAX)return 1;
    struct proc_bsdinfo value={0};if(proc_pidinfo((int)number,PROC_PIDTBSDINFO,0,&value,sizeof(value))!=(int)sizeof(value)){perror("Supervisor process identity");return 1;}
    printf("{\"pid\":%u,\"parentPid\":%u,\"uid\":%u,\"startedSeconds\":%llu,\"startedMicros\":%llu}\n",value.pbi_pid,value.pbi_ppid,value.pbi_uid,(unsigned long long)value.pbi_start_tvsec,(unsigned long long)value.pbi_start_tvusec);return 0;
}
static int namespace_processes(const char *path){
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){perror("Supervisor namespace directory");return 1;}
    struct stat status;char physical[PATH_MAX];
    if(fstat(fd,&status)||!S_ISDIR(status.st_mode)||status.st_uid!=getuid()||(status.st_mode&077)||fcntl(fd,F_GETPATH,physical)<0){fputs("Supervisor namespace directory is not private\n",stderr);close(fd);return 1;}
    int required=proc_listpidspath(PROC_UID_ONLY,getuid(),physical,0,NULL,0);if(required<=0){close(fd);return 1;}
    size_t capacity=(size_t)required+32*sizeof(pid_t);pid_t *pids=NULL;int length=-1;
    for(unsigned attempt=0;attempt<3;attempt++){
        if(capacity>INT32_MAX)break;
        pids=malloc(capacity);if(!pids)break;
        int bytes=proc_listpidspath(PROC_UID_ONLY,getuid(),physical,0,pids,(int)capacity);
        if(bytes>=0&&(size_t)bytes<capacity&&bytes%(int)sizeof(pid_t)==0){length=bytes/(int)sizeof(pid_t);break;}
        free(pids);pids=NULL;if(bytes<0)break;capacity*=2;
    }
    if(length<0){free(pids);close(fd);return 1;}
    // This short-lived selector holds the directory to validate its scope;
    // it has no Android identity and is never a candidate for guest discovery.
    printf("{\"pids\":[");int comma=0;
    for(int i=0;i<length;i++)if(pids[i]>0&&pids[i]!=getpid()){printf("%s%d",comma?",":"",pids[i]);comma=1;}
    puts("]}");free(pids);close(fd);return 0;
}
static int locked_exec(int argc,char**argv){
    if(argc<4)return 1;
    int lock=open(argv[2],O_RDWR|O_CREAT|O_NOFOLLOW,0600);if(lock<0){perror("Supervisor owner lock");return 1;}
    struct stat status;
    if(fstat(lock,&status)||!S_ISREG(status.st_mode)||status.st_uid!=getuid()||(status.st_mode&077)){fputs("Supervisor owner lock is not private\n",stderr);close(lock);return 1;}
    if(flock(lock,LOCK_EX|LOCK_NB)){int code=errno==EWOULDBLOCK?73:1;close(lock);return code;}
    // The exec'ed supervisor is the sole kernel lock owner. No child inherits
    // this descriptor unless that supervisor explicitly passes it as stdio.
    char descriptor[32];snprintf(descriptor,sizeof(descriptor),"%d",lock);setenv("MACLAUNCHER_ANDROID_OWNER_FD",descriptor,1);
    execv(argv[3],argv+3);perror("Supervisor executable");close(lock);return 1;
}
static int transfer(int channel,int descriptor,int acknowledgement){
    uint32_t size;if(read_all(0,&size,sizeof(size)))return -1;size_t length=ntohl(size);if(length<2||length>MAX_FRAME)return -1;
    char *frame=malloc(length+4);if(!frame)return -1;memcpy(frame,&size,4);if(read_all(0,frame+4,length)){free(frame);return -1;}
    union{struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int)*2)];}control={0};
    struct iovec iov={.iov_base=frame,.iov_len=length+4};struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int)*2);int descriptors[2]={descriptor,acknowledgement};memcpy(CMSG_DATA(header),descriptors,sizeof(descriptors));
    ssize_t count;for(;;){count=sendmsg(channel,&message,0);if(count>=0)break;if(errno==EINTR)continue;if((errno==EAGAIN||errno==EWOULDBLOCK)&&!ready(channel,POLLOUT))continue;free(frame);return -1;}
    int result=write_all(channel,frame+count,length+4-(size_t)count);free(frame);return result;
}
int main(int argc,char**argv){
    if(argc==2&&!strcmp(argv[1],"--peer"))return peer_identity();
    if(argc==3&&!strcmp(argv[1],"--process"))return process_identity(argv[2]);
    if(argc==3&&!strcmp(argv[1],"--namespace-processes"))return namespace_processes(argv[2]);
    if(argc>=4&&!strcmp(argv[1],"--lock"))return locked_exec(argc,argv);
    if(argc!=1){fputs("Invalid Android supervisor operation\n",stderr);return 1;}
    signal(SIGPIPE,SIG_IGN);if(!socket_ok(4)||!socket_ok(5)){fputs("Supervisor descriptor is not a Unix stream socket\n",stderr);return 1;}
    alarm(30); // A stalled/dead receiver cannot retain a handoff indefinitely.
    int pair[2],receipt[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair)){perror("Supervisor socket pair");return 1;}
    if(pipe(receipt)){close(pair[0]);close(pair[1]);perror("Supervisor receipt pipe");return 1;}
    // Data endpoints never enter a Node stream owner. Each receiver acquires
    // its independent descriptor before these native sender references close.
    int result=transfer(4,pair[0],receipt[1]);if(!result)result=transfer(5,pair[1],receipt[1]);close(receipt[1]);
    // Darwin can disconnect Unix sockets whose only remaining references are
    // still queued SCM_RIGHTS messages. Retire sender references only after
    // both receivers have installed their independent descriptors.
    char accepted[2];if(!result)result=read_all(receipt[0],accepted,sizeof(accepted));if(!result&&(accepted[0]!=1||accepted[1]!=1)){errno=EPROTO;result=-1;}close(receipt[0]);
    if(result)perror("Supervisor descriptor transfer");close(pair[0]);close(pair[1]);return result?1:0;
}
#else
#include <jni.h>
JNIEXPORT jobject JNICALL Java_maclauncher_android_ProcessAuthority_receiveNative(JNIEnv*env,jclass unused,jint descriptor){
    (void)unused;int passed=-1,acknowledgement=-1;uint32_t header=0;size_t at=0;int eof=0;int socket=fcntl(descriptor,F_DUPFD_CLOEXEC,0);
    if(socket<0)goto failed;
    if(!socket_ok(socket)){errno=ENOTSOCK;goto failed;}
    while(at<sizeof(header)){
        union{struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int)*4)];}control={0};
        struct iovec iov={.iov_base=(char*)&header+at,.iov_len=sizeof(header)-at};struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
        ssize_t count=recvmsg(socket,&message,0);if(count<0&&errno==EINTR)continue;if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&!ready(socket,POLLIN))continue;if(count<=0){if(!count){eof=at==0;errno=EPIPE;}goto failed;}
        int invalid=(message.msg_flags&MSG_CTRUNC)!=0;
        for(struct cmsghdr*c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c))if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS){
            size_t entries=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);int*fds=(int*)CMSG_DATA(c);for(size_t i=0;i<entries;i++){if(passed<0)passed=fds[i];else if(acknowledgement<0)acknowledgement=fds[i];else{close(fds[i]);invalid=1;}}
        }
        if(invalid){errno=EPROTO;goto failed;}at+=(size_t)count;
    }
    size_t length=ntohl(header);if(length<2||length>MAX_FRAME){errno=EMSGSIZE;goto failed;}
    char *bytes=malloc(length);if(!bytes){errno=ENOMEM;goto failed;}if(read_all(socket,bytes,length)){free(bytes);goto failed;}
    if(passed>=0&&(fcntl(passed,F_SETFD,FD_CLOEXEC)<0||!socket_ok(passed))){free(bytes);errno=ENOTSOCK;goto failed;}
    if(passed>=0){struct stat status;if(acknowledgement<0||fstat(acknowledgement,&status)||!S_ISFIFO(status.st_mode)){free(bytes);errno=EPROTO;goto failed;}}
    jbyteArray data=(*env)->NewByteArray(env,(jsize)length);if(!data){free(bytes);if(passed>=0)close(passed);if(acknowledgement>=0)close(acknowledgement);close(socket);return NULL;}(*env)->SetByteArrayRegion(env,data,0,(jsize)length,(jbyte*)bytes);free(bytes);
    jclass type=(*env)->FindClass(env,"maclauncher/android/ProcessAuthority$Packet");jmethodID ctor=type?(*env)->GetMethodID(env,type,"<init>","([BI)V"):NULL;
    jobject result=ctor?(*env)->NewObject(env,type,ctor,data,passed):NULL;
    // The received socket already has its own kernel reference. A sender may
    // time out or lose its other receiver before this receipt is written;
    // that transfer's receipt cannot retire the independent authority stream.
    // The data connection and the parent's unavailable message retain their
    // existing per-peer failure handling.
    if(result&&acknowledgement>=0){char accepted=1;(void)write_all(acknowledgement,&accepted,1);}
    if(!result&&passed>=0)close(passed);if(acknowledgement>=0)close(acknowledgement);close(socket);return result;
failed:
    {int saved=errno;if(passed>=0)close(passed);if(acknowledgement>=0)close(acknowledgement);if(socket>=0)close(socket);if(eof)return NULL;jclass errorType=(*env)->FindClass(env,"java/io/IOException");if(errorType)(*env)->ThrowNew(env,errorType,strerror(saved));return NULL;}
}
JNIEXPORT void JNICALL Java_maclauncher_android_ProcessAuthority_shutdownNative(JNIEnv*env,jclass type,jint socket){(void)env;(void)type;shutdown(socket,SHUT_RDWR);}
#endif
