#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(v) do{if(!(v)){fprintf(stderr,"JIT user-copy line %d errno %d\n",__LINE__,errno);return __LINE__;}}while(0)
int main(int argc,char **argv){
 size_t page=(size_t)sysconf(_SC_PAGESIZE);
 unsigned char *p=mmap(0,page*3,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(p!=MAP_FAILED);memset(p,0x5a,page*3);
 CHECK(mprotect(p,page,PROT_READ|PROT_WRITE|PROT_EXEC)==0);
 struct stat *out=(struct stat *)(p+32);
 CHECK(stat("/",out)==0&&S_ISDIR(out->st_mode));
 for(int i=0;i<32;i++)CHECK(p[i]==0x5a);
 for(size_t i=32+sizeof(*out);i<page;i++)CHECK(p[i]==0x5a);
 int fd=open("/",O_RDONLY|O_DIRECTORY);CHECK(fd>=0);
 CHECK(fstat(fd,out)==0&&S_ISDIR(out->st_mode));
 // A JIT source remains readable by the original Mach-backed user-copy path.
 strcpy((char*)p+512,"/");CHECK(stat((char*)p+512,out)==0);
 uint64_t mask=0x200,observed=0;
 CHECK(syscall(__NR_rt_sigprocmask,2,&mask,p+1024,8)==0);
 CHECK(syscall(__NR_rt_sigprocmask,0,0,&observed,8)==0&&observed==mask);
 CHECK(syscall(__NR_rt_sigprocmask,2,p+1024,0,8)==0);
 CHECK(mprotect(p+page,page,PROT_NONE)==0);
 memset(p+page-64,0x6c,64);errno=0;
 CHECK(stat("/",(struct stat*)(p+page-64))==-1&&errno==EFAULT);
 for(int i=0;i<64;i++)CHECK(p[page-64+i]==0x6c);
 CHECK(mprotect(p,page,PROT_READ)==0);errno=0;
 CHECK(fstat(fd,out)==-1&&errno==EFAULT);
 errno=0;CHECK(stat((char*)1,(struct stat*)(p+page*2))==-1&&errno==EFAULT);
 CHECK(mprotect(p,page,PROT_READ|PROT_WRITE|PROT_EXEC)==0);
 struct iovec memory_local={p+512,4},memory_remote={(void*)"self",4};
 CHECK(syscall(__NR_process_vm_readv,getpid(),&memory_local,1,&memory_remote,1,0)==4&&!memcmp(p+512,"self",4));
 memory_local=(struct iovec){(void*)"copy",4};memory_remote=(struct iovec){p+516,4};
 CHECK(syscall(__NR_process_vm_writev,getpid(),&memory_local,1,&memory_remote,1,0)==4&&!memcmp(p+516,"copy",4));
 int data=open("/data/local/tmp/signal-probe/io.data",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(data>=0);
 CHECK(write(data,"abcdefghij",10)==10);CHECK(lseek(data,0,SEEK_SET)==0);
 CHECK(read(data,p+128,4)==4&&!memcmp(p+128,"abcd",4));CHECK(lseek(data,0,SEEK_CUR)==4);
 CHECK(pread(data,p+128,4,6)==4&&!memcmp(p+128,"ghij",4));CHECK(lseek(data,0,SEEK_CUR)==4);
 unsigned char a[2]={0},b[2]={0};struct iovec vectors[3]={{a,2},{p+256,2},{b,2}};
 CHECK(readv(data,vectors,3)==6&&!memcmp(a,"ef",2)&&!memcmp(p+256,"gh",2)&&!memcmp(b,"ij",2));
 CHECK(read(data,p+128,8)==0);CHECK(lseek(data,0,SEEK_SET)==0);
 int pipefd[2];CHECK(pipe(pipefd)==0);CHECK(write(pipefd[1],"pipe!",5)==5);
 vectors[0]=(struct iovec){a,2};vectors[1]=(struct iovec){p+256,8};p[259]=0x71;
 CHECK(readv(pipefd[0],vectors,2)==5&&!memcmp(a,"pi",2)&&!memcmp(p+256,"pe!",3)&&p[259]==0x71);
 CHECK(close(pipefd[0])==0&&close(pipefd[1])==0);
 int sockets[2];CHECK(socketpair(AF_UNIX,(argc>1&&!strcmp(argv[1],"--datagram-truncation"))?SOCK_DGRAM:SOCK_SEQPACKET,0,sockets)==0);CHECK(send(sockets[0],"hello",5,0)==5);
 vectors[0]=(struct iovec){p+256,3};vectors[1]=(struct iovec){a,1};
 struct msghdr *message=(struct msghdr*)(p+2048);memset(message,0,sizeof(*message));message->msg_iov=vectors;message->msg_iovlen=2;
 ssize_t received=recvmsg(sockets[1],message,MSG_TRUNC);fprintf(stderr,"JIT packet result=%zd flags=%x payload=%c%c%c/%c errno=%d\n",received,message->msg_flags,p[256],p[257],p[258],a[0],errno);
 CHECK(received==5&&!memcmp(p+256,"hel",3)&&a[0]=='l'&&(message->msg_flags&MSG_TRUNC));
 CHECK(send(sockets[0],"xy",2,0)==2);vectors[0]=(struct iovec){p+256,2};vectors[1]=(struct iovec){(void*)1,64};
 message->msg_flags=0;CHECK(recvmsg(sockets[1],message,0)==2&&!memcmp(p+256,"xy",2));
 CHECK(close(sockets[0])==0&&close(sockets[1])==0);CHECK(close(data)==0);
 CHECK(close(fd)==0);CHECK(munmap(p,page*3)==0);
 errno=0;CHECK(stat("/",out)==-1&&errno==EFAULT);
 puts("PASS original Bionic stat/fstat, signal, file, vector, pipe and socket JIT outputs retain counts and EFAULT");return 0;
}
