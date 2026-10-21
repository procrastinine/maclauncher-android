#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static void byte(int fd,char value){assert(write(fd,&value,1)==1);}
static char next(int fd){char value=0;assert(read(fd,&value,1)==1);return value;}
static int dumpable(void){return prctl(PR_GET_DUMPABLE,0L,0L,0L,0L);}
static void set_dumpable(int value){assert(prctl(PR_SET_DUMPABLE,(long)value,0L,0L,0L)==0);}
static const char payload[]="original-peer-memory";
static void maps(pid_t target,int allowed){
 char file[80],data[128];snprintf(file,sizeof file,"/proc/%d/maps",target);
 int fd=open(file,O_RDONLY);if(!allowed){assert(fd<0);return;}
 assert(fd>=0&&read(fd,data,sizeof data)>0);assert(close(fd)==0);
}
int main(int argc,char **argv){
 alarm(25);
 if(argc==2&&!strcmp(argv[1],"--exec")){
  assert(dumpable()==1);
  puts("PASS original ART prctl, effective credential reset, authenticated peer memory/maps denial after tracer attach, fork inheritance and exec reset");
  return 0;
 }
 assert(dumpable()==1);
 errno=0;assert(prctl(PR_SET_DUMPABLE,2L,0L,0L,0L)==-1&&errno==EINVAL);
 assert(dumpable()==1);
 assert(setresgid(10023,10023,10023)==0&&dumpable()==0);set_dumpable(1);
 assert(setresuid(10023,10023,10023)==0&&dumpable()==0);set_dumpable(1);
 pid_t parent=getpid();int commands[2],replies[2];assert(!pipe(commands)&&!pipe(replies));
 pid_t child=fork();assert(child>=0);
 if(!child){
  close(commands[1]);close(replies[0]);assert(dumpable()==1);
  // Establish a real tracer relationship before the target changes policy.
  assert(ptrace(PTRACE_SEIZE,parent,NULL,0)==0);
  char output[sizeof payload]={0};struct iovec local={output,sizeof output},remote={(void *)payload,sizeof payload};
  ssize_t copied=process_vm_readv(parent,&local,1,&remote,1,0);
  if(copied!=(ssize_t)sizeof payload)fprintf(stderr,"initial peer read: result=%zd errno=%d\n",copied,errno);
  assert(copied==(ssize_t)sizeof payload&&!memcmp(output,payload,sizeof payload));
  maps(parent,1);byte(replies[1],'r');assert(next(commands[0])=='d');
  errno=0;assert(process_vm_readv(parent,&local,1,&remote,1,0)==-1&&errno==EPERM);
  errno=0;assert(process_vm_writev(parent,&local,1,&remote,1,0)==-1&&errno==EPERM);
  maps(parent,0);byte(replies[1],'d');assert(next(commands[0])=='e');
  assert(process_vm_readv(parent,&local,1,&remote,1,0)==sizeof payload);maps(parent,1);
  set_dumpable(0);assert(process_vm_readv(getpid(),&local,1,&remote,1,0)==sizeof payload);
  pid_t nested=fork();assert(nested>=0);if(!nested)_exit(dumpable()==0?0:91);
  int status;assert(waitpid(nested,&status,0)==nested&&WIFEXITED(status)&&!WEXITSTATUS(status));
  byte(replies[1],'e');_exit(0);
 }
 close(commands[0]);close(replies[1]);assert(next(replies[0])=='r');
 set_dumpable(0);byte(commands[1],'d');assert(next(replies[0])=='d');
 set_dumpable(1);byte(commands[1],'e');assert(next(replies[0])=='e');
 int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 close(commands[1]);close(replies[0]);set_dumpable(0);
 execl(argv[0],argv[0],"--exec",NULL);return 93;
}
