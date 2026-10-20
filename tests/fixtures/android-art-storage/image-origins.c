#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static const char *names[]={"/usr/share/maclauncher-image-names/system/libsame.so","/usr/share/maclauncher-image-names/apex/libsame.so"};
static void name(int fd,const char *expected){
 char proc[64],actual[PATH_MAX];snprintf(proc,sizeof proc,"/proc/self/fd/%d",fd);
 ssize_t count=readlink(proc,actual,sizeof actual-1);assert(count>=0);actual[count]=0;
 if(strcmp(actual,expected)){fprintf(stderr,"opened image %d: %s != %s\n",fd,actual,expected);abort();}
}
static void canonical(const char *expected){
 char *actual=realpath(expected,NULL);
 if(!actual||strcmp(actual,expected)){fprintf(stderr,"image realpath: %s != %s (errno %d)\n",actual?actual:"NULL",expected,errno);abort();}free(actual);
}
int main(int argc,char **argv){
 if(argc==4&&!strcmp(argv[1],"--inherited")){
  name(atoi(argv[2]),names[0]);name(atoi(argv[3]),names[1]);
  puts("PASS immutable image origins survive exec");return 0;
 }
 int a=open(names[0],O_RDONLY),b=open(names[1],O_RDONLY);assert(a>=0&&b>=0);
 for(unsigned n=0;n<16;n++){
  name(a,names[0]);name(b,names[1]);canonical(names[n&1]);
  int next=open(names[n&1],O_RDONLY);assert(next>=0);name(next,names[n&1]);assert(!close(next));
 }
 int copy=fcntl(a,F_DUPFD,100);assert(copy>=0);assert(!close(a));name(copy,names[0]);
 int reused=open(names[1],O_RDONLY);assert(reused>=0);name(reused,names[1]);name(copy,names[0]);assert(!close(reused));
 int target=dup(copy);assert(target>=0);assert(dup2(b,target)==target);name(target,names[1]);name(copy,names[0]);assert(!close(target));
 pid_t child=fork();assert(child>=0);
 if(!child){char x[32],y[32];name(copy,names[0]);name(b,names[1]);snprintf(x,sizeof x,"%d",copy);snprintf(y,sizeof y,"%d",b);execl(argv[0],argv[0],"--inherited",x,y,NULL);perror("exec");_exit(1);}
 int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 assert(!close(copy)&&!close(b));
 // Actual image libraries are byte-identical hardlinks across these APEX and
 // system names. Their original namespace checks must see the opened name.
 canonical("/system/lib64/libc++.so");canonical("/apex/com.android.hardware.power/lib64/libc++.so");canonical("/system/lib64/libc++.so");
 canonical("/system/lib64/android.hardware.media.c2-V1-ndk.so");
 canonical("/apex/com.android.media.swcodec/lib64/android.hardware.media.c2-V1-ndk.so");
 canonical("/system/lib64/android.hardware.media.c2-V1-ndk.so");
 puts("PASS original Bionic realpath and descriptor origins for shared immutable image/cache bytes, dup, fcntl, close/reuse, fork and exec");
}
