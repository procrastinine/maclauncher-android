#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
typedef ssize_t(*transfer)(int,int,off_t *,size_t);
static char bytes[262144];
static int temporary(void){char file[]="/tmp/android-native-transfer-XXXXXX";int fd=mkstemp(file);assert(fd>=0);assert(!unlink(file));return fd;}
static int source(void){int fd=temporary();assert(write(fd,bytes,sizeof bytes)==sizeof bytes);assert(lseek(fd,0,SEEK_SET)==0);return fd;}
static void nonblocking(int fd){int flags=fcntl(fd,F_GETFL);assert(flags>=0);assert(!fcntl(fd,F_SETFL,flags|O_NONBLOCK));}
static void pair(int fds[2]){assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));int size=4096;assert(!setsockopt(fds[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof size));nonblocking(fds[0]);}
static void discard(int fd,size_t length){char buffer[4096];while(length){size_t want=length<sizeof buffer?length:sizeof buffer;ssize_t got=read(fd,buffer,want);assert(got>0);length-=(size_t)got;}}
static void verify(int fd,off_t start,size_t length){char got[262144];assert(length<=sizeof got);size_t total=0;while(total<length){ssize_t n=read(fd,got+total,length-total);assert(n>0);total+=(size_t)n;}assert(!memcmp(got,bytes+start,length));}
int main(int argc,char **argv){
 assert(argc==5);for(size_t i=0;i<sizeof bytes;i++)bytes[i]=(char)(i*37+11);
 void *core=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!core){puts(dlerror());return 2;}
 void *(*lookup)(const char *)=dlsym(core,"kl_shim_lookup");assert(lookup);
 int (*configure)(const char *)=dlsym(core,"klj_process_ids_configure");void(*enter)(void)=dlsym(core,"kl_thread_init");assert(configure&&enter);assert(configure(argv[3])>0);enter();
 if(!strcmp(argv[4],"baseline")){
  int a=lookup("posix_madvise")==NULL,b=lookup("sendfile64")==NULL;assert(a&&b);
  transfer old=lookup("sendfile");assert(old);int in=source(),fds[2];pair(fds);
  ssize_t n=old(fds[0],in,NULL,65536);off_t at=lseek(in,0,SEEK_CUR);assert(n>0&&n<65536&&at>n);
  printf("BASELINE missing posix_madvise/sendfile64; sendfile short-write returned=%zd inputOffset=%lld (read-ahead loss=%lld)\n",n,(long long)at,(long long)(at-n));
  close(in);close(fds[0]);close(fds[1]);return 0;
 }
 assert(lookup("posix_madvise")&&lookup("sendfile64"));
 void *(*load)(const char *)=dlsym(core,"kl_load");void *(*symbol)(void *,const char *)=dlsym(core,"kl_sym");const char *(*error)(void)=dlsym(core,"kl_error");assert(load&&symbol&&error);
 void *image=load(argv[2]);if(!image){puts(error());return 3;}
 int(*advice)(void *,size_t,int)=symbol(image,"guest_advice");int(*guest_errno)(void)=symbol(image,"guest_errno");transfer basic=symbol(image,"guest_sendfile"),wide=symbol(image,"guest_sendfile64");assert(advice&&guest_errno&&basic&&wide);
 size_t page=(size_t)sysconf(_SC_PAGESIZE);void *memory=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);assert(memory!=MAP_FAILED);memset(memory,0xa5,page);
 errno=EDOM;assert(!advice(memory,page,4));assert(guest_errno()==33);for(size_t i=0;i<page;i++)assert(((unsigned char*)memory)[i]==0xa5);
 errno=EDOM;assert(advice((char*)memory+1,page,0)==22);assert(guest_errno()==33);
 errno=EDOM;assert(advice(memory,page,31)==22);assert(guest_errno()==33);errno=EDOM;assert(!advice(memory,0,0));assert(guest_errno()==33);
 assert(!munmap(memory,page));errno=EDOM;assert(advice(memory,page,0)==12);assert(guest_errno()==33);
 puts("PASS original Android posix_madvise preserves errno/private bytes and returns Linux errors");
 int in=source(),out=temporary();assert(lseek(in,17,SEEK_SET)==17);off_t offset=13;
 assert(wide(out,in,&offset,23)==23&&offset==36&&lseek(in,0,SEEK_CUR)==17);assert(lseek(out,0,SEEK_SET)==0);verify(out,13,23);
 assert(lseek(out,0,SEEK_SET)==0);assert(basic(out,in,NULL,31)==31&&lseek(in,0,SEEK_CUR)==48);assert(lseek(out,0,SEEK_SET)==0);verify(out,17,31);
 offset=sizeof bytes;assert(!wide(out,in,&offset,19)&&offset==sizeof bytes&&lseek(in,0,SEEK_CUR)==48);
 offset=-1;assert(wide(out,in,&offset,1)==-1&&guest_errno()==22&&offset==-1);
 off_t *invalid=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);assert(invalid!=MAP_FAILED);*invalid=13;
 off_t input_before=lseek(in,0,SEEK_CUR),output_before=lseek(out,0,SEEK_CUR);assert(!mprotect(invalid,page,PROT_READ));
 assert(wide(out,in,invalid,1)==-1&&guest_errno()==14&&*invalid==13);assert(lseek(in,0,SEEK_CUR)==input_before&&lseek(out,0,SEEK_CUR)==output_before);
 assert(!mprotect(invalid,page,PROT_NONE));assert(wide(out,in,invalid,1)==-1&&guest_errno()==14);
 assert(!munmap(invalid,page));assert(wide(out,in,invalid,1)==-1&&guest_errno()==14);assert(wide(out,in,(off_t*)1,1)==-1&&guest_errno()==14);
 assert(lseek(in,0,SEEK_CUR)==input_before&&lseek(out,0,SEEK_CUR)==output_before);
 puts("PASS invalid/read-only offset pointers return EFAULT before transfer without native fault");
 int flags=fcntl(out,F_GETFL);assert(!fcntl(out,F_SETFL,flags|O_APPEND));assert(wide(out,in,NULL,1)==-1&&guest_errno()==22);close(out);close(in);
 puts("PASS original Android sendfile names preserve explicit/current offsets, file output, EOF and errors");
 for(int mode=0;mode<2;mode++){
  int fd=source(),p[2];assert(!pipe(p));nonblocking(p[1]);
  char padding[4096]={0};size_t filled=0;ssize_t written;while((written=write(p[1],padding,sizeof padding))>0)filled+=(size_t)written;assert(errno==EAGAIN&&filled>=8192);discard(p[0],8192);
  offset=mode?7:0;ssize_t n=wide(p[1],fd,mode?&offset:NULL,65536);assert(n>0&&n<65536);
  assert(lseek(fd,0,SEEK_CUR)==(mode?0:n));if(mode)assert(offset==7+n);discard(p[0],filled-8192);verify(p[0],mode?7:0,(size_t)n);
  printf("PASS nonblocking pipe partial mode=%d returned=%zd sourceOffset=%lld\n",mode,n,(long long)lseek(fd,0,SEEK_CUR));close(p[0]);close(p[1]);close(fd);
 }
 int fd=source(),p[2];pair(p);offset=9;ssize_t n=wide(p[0],fd,&offset,sizeof bytes);assert(n>0&&(size_t)n<sizeof bytes&&offset==9+n&&lseek(fd,0,SEEK_CUR)==0);verify(p[1],9,(size_t)n);
 char fill[4096]={0};while(write(p[0],fill,sizeof fill)>0){}assert(errno==EAGAIN);off_t before=lseek(fd,0,SEEK_CUR);assert(wide(p[0],fd,NULL,32)==-1&&guest_errno()==11&&lseek(fd,0,SEEK_CUR)==before);
 printf("PASS nonblocking socket partial returned=%zd; EAGAIN leaves source offset unchanged\n",n);close(p[0]);close(p[1]);close(fd);
 puts("PASS actual Android-produced ELF calls corrected POSIX advice and Linux sendfile ABI");return 0;
}
