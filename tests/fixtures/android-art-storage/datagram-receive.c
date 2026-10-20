#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static double now(void){struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return t.tv_sec+t.tv_nsec/1e9;}
static void control(int fd,int sender){
 int pipefd[2];assert(pipe(pipefd)==0);int enabled=1;assert(setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&enabled,sizeof enabled)==0);
 char data[]="hello",out[4];struct iovec vector={data,5};
 union{struct cmsghdr alignment;char bytes[CMSG_SPACE(sizeof(int))+CMSG_SPACE(sizeof(struct ucred))];} ancillary;
 memset(&ancillary,0,sizeof ancillary);struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=ancillary.bytes,.msg_controllen=CMSG_SPACE(sizeof(int))};
 struct cmsghdr*header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&pipefd[0],sizeof(int));assert(sendmsg(sender,&message,0)==5);
 vector=(struct iovec){out,sizeof out};message.msg_controllen=sizeof ancillary;assert(recvmsg(fd,&message,MSG_TRUNC)==5&&!memcmp(out,"hell",4)&&(message.msg_flags&MSG_TRUNC)&&!(message.msg_flags&MSG_CTRUNC));
 int received=-1,saw_credentials=0;
 for(header=CMSG_FIRSTHDR(&message);header;header=CMSG_NXTHDR(&message,header)){
  assert(header->cmsg_level==SOL_SOCKET);
  if(header->cmsg_type==SCM_RIGHTS){assert(received==-1&&header->cmsg_len==CMSG_LEN(sizeof(int)));memcpy(&received,CMSG_DATA(header),sizeof(int));}
  else if(header->cmsg_type==SCM_CREDENTIALS){struct ucred credentials;assert(header->cmsg_len==CMSG_LEN(sizeof credentials));memcpy(&credentials,CMSG_DATA(header),sizeof credentials);assert(credentials.pid==getpid()&&credentials.uid==getuid()&&credentials.gid==getgid());saw_credentials=1;}
 }
 assert(received>=0&&saw_credentials);assert(write(pipefd[1],"x",1)==1&&read(received,out,1)==1&&out[0]=='x');assert(close(received)==0&&close(pipefd[0])==0&&close(pipefd[1])==0);enabled=0;assert(setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&enabled,sizeof enabled)==0);
}
static void simple(int fd,int sender){
 char b[8]={0};struct iovec vectors[2]={{b,2},{(void*)1,64}};
 struct msghdr message={.msg_iov=vectors,.msg_iovlen=2};
 assert(send(sender,"xy",2,0)==2);assert(recvmsg(fd,&message,MSG_TRUNC)==2&&!memcmp(b,"xy",2));
 assert(send(sender,"hello",5,0)==5);vectors[0].iov_len=2;message.msg_iovlen=1;
 assert(recvmsg(fd,&message,MSG_TRUNC|MSG_PEEK)==5&&(message.msg_flags&MSG_TRUNC)&&!memcmp(b,"he",2));
 vectors[0].iov_len=3;assert(recvmsg(fd,&message,0)==3&&(message.msg_flags&MSG_TRUNC)&&!memcmp(b,"hel",3));
 assert(send(sender,"",0,0)==0);vectors[0].iov_len=0;assert(recvmsg(fd,&message,MSG_TRUNC)==0);
 struct timeval timeout={0,100000};assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)==0);
 double start=now();errno=0;assert(recv(fd,b,1,0)==-1&&errno==EAGAIN);assert(now()-start>=0.07&&now()-start<1.0);
 start=now();errno=0;assert(recv(fd,b,1,MSG_DONTWAIT)==-1&&errno==EAGAIN&&now()-start<0.05);
 timeout=(struct timeval){0};assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)==0);
}
enum{COUNT=80};
static void*sender(void*p){int fd=*(int*)p;for(unsigned i=0;i<COUNT;i++){uint32_t record[32];unsigned size=5+i%100;memset(record,(int)i,sizeof record);record[0]=size;assert(send(fd,record,size,0)==size);}return NULL;}
static void receiver(int fd,int full){for(int i=0;i<COUNT/2;i++){uint32_t record[32]={0};struct iovec v={record,full?sizeof record:sizeof record[0]};struct msghdr m={.msg_iov=&v,.msg_iovlen=1};ssize_t n=recvmsg(fd,&m,full?0:MSG_TRUNC);uint32_t size=record[0];if(!(size>=5&&size<=104&&n==size&&!!(m.msg_flags&MSG_TRUNC)==!full))fprintf(stderr,"datagram receiver pid=%d index=%d returned=%zd size=%u flags=%x errno=%d\n",getpid(),i,n,size,m.msg_flags,errno);assert(size>=5&&size<=104&&n==size&&!!(m.msg_flags&MSG_TRUNC)==!full);}}
int main(void){
 int pair[2];assert(socketpair(AF_UNIX,SOCK_DGRAM,0,pair)==0);simple(pair[1],pair[0]);control(pair[1],pair[0]);
 // A child restores the same receipt owner before lowering its limit. No
 // receive may need a descriptor allocation after this point.
 pid_t child=fork();if(child<0)perror("original Bionic fork");assert(child>=0);
 if(child==0){struct rlimit zero={0,0};assert(setrlimit(RLIMIT_NOFILE,&zero)==0);receiver(pair[1],0);_exit(0);}
 pthread_t thread;assert(pthread_create(&thread,NULL,sender,&pair[0])==0);receiver(pair[1],1);assert(pthread_join(thread,NULL)==0);
 int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 assert(close(pair[0])==0&&close(pair[1])==0);
 puts("PASS original Bionic datagram lengths, peek, short suffix, timeout, SCM/credentials and shared fork-NOFILE0 receivers");return 0;
}
