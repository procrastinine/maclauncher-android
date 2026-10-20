#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(v) do{if(!(v)){fprintf(stderr,"JIT user-copy line %d errno %d\n",__LINE__,errno);return __LINE__;}}while(0)
int main(void){
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
 CHECK(close(fd)==0);CHECK(munmap(p,page*3)==0);
 errno=0;CHECK(stat("/",out)==-1&&errno==EFAULT);
 puts("PASS original Bionic stat/fstat and signal copies support JIT buffers and retain EFAULT");return 0;
}
