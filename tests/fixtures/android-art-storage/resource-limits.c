#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <string.h>

int main(int argc,char **argv){
 assert(argc==3);uint64_t ceiling=strtoull(argv[1],NULL,10);assert(ceiling>0);
 struct rlimit original,raised,core,unchanged;
 assert(getrlimit(RLIMIT_CORE,&core)==0&&getrlimit(RLIMIT_NOFILE,&original)==0);
 printf("initial nofile %llu %llu ceiling %llu\n",(unsigned long long)original.rlim_cur,(unsigned long long)original.rlim_max,(unsigned long long)ceiling);
 assert(original.rlim_cur<=original.rlim_max&&original.rlim_max<=ceiling);
 // Ordinary native startup raises soft to the advertised hard limit. This
 // must remain finite and usable as a descriptor-table element count.
 raised.rlim_cur=raised.rlim_max=original.rlim_max;assert(setrlimit(RLIMIT_NOFILE,&raised)==0);
 struct rlimit current;assert(getrlimit(RLIMIT_NOFILE,&current)==0&&current.rlim_cur==raised.rlim_cur&&current.rlim_max==raised.rlim_max);
 uint64_t alias[2]={0};assert(syscall(__NR_prlimit64,0,RLIMIT_NOFILE,NULL,alias)==0&&alias[0]==current.rlim_cur&&alias[1]==current.rlim_max);
 assert(current.rlim_cur>0&&current.rlim_cur<=INT32_MAX/8);
 int32_t tableBytes=(int32_t)((uint32_t)current.rlim_cur*8);assert(tableBytes>0&&(uint64_t)tableBytes<=ceiling*8);
 struct rlimit invalid={1,ceiling+1};errno=0;assert(setrlimit(RLIMIT_NOFILE,&invalid)==-1&&errno==EPERM);
 assert(getrlimit(RLIMIT_NOFILE,&current)==0&&current.rlim_cur==raised.rlim_cur&&current.rlim_max==raised.rlim_max);
 assert(fflush(stdout)==0);
 int pipes[2],sockets[2];assert(pipe(pipes)==0&&socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
 struct rlimit lower={64,64};assert(setrlimit(RLIMIT_NOFILE,&lower)==0&&getrlimit(RLIMIT_NOFILE,&current)==0&&current.rlim_cur==64&&current.rlim_max==64);
 // No privilege may raise NOFILE above the real kernel ceiling. Without
 // CAP_SYS_RESOURCE a lower explicit hard limit also cannot be raised.
 if(getuid()!=0||strtoul(argv[2],NULL,10)!=0){
  invalid.rlim_cur=invalid.rlim_max=65;errno=0;assert(setrlimit(RLIMIT_NOFILE,&invalid)==-1&&errno==EPERM);
  assert(getrlimit(RLIMIT_NOFILE,&current)==0&&current.rlim_cur==64&&current.rlim_max==64);
 }
 assert(getrlimit(RLIMIT_CORE,&unchanged)==0&&core.rlim_cur==unchanged.rlim_cur&&core.rlim_max==unchanged.rlim_max);
 assert(write(pipes[1],"lower",5)==5);char data[8]={0};assert(read(pipes[0],data,5)==5&&!memcmp(data,"lower",5));
 lower.rlim_cur=lower.rlim_max=0;assert(setrlimit(RLIMIT_NOFILE,&lower)==0);
 assert(getrlimit(RLIMIT_NOFILE,&current)==0&&current.rlim_cur==0&&current.rlim_max==0);
 assert(write(pipes[1],"zero",4)==4&&read(pipes[0],data,4)==4&&!memcmp(data,"zero",4));
 assert(write(sockets[0],"socket",6)==6&&read(sockets[1],data,6)==6&&!memcmp(data,"socket",6));
 errno=0;assert(dup(pipes[0])==-1&&errno==EMFILE);
 assert(close(pipes[1])==0&&read(pipes[0],data,1)==0&&close(pipes[0])==0);
 assert(close(sockets[0])==0&&read(sockets[1],data,1)==0&&close(sockets[1])==0);
 puts("PASS original Bionic NOFILE bounds, raise-to-hard, prlimit64, lower/zero I/O and permission errors");assert(fflush(stdout)==0);return 0;
}
