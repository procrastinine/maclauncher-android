#include "process-abi/prctl.h"
#include "process-memory.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void byte(int fd,char value){assert(write(fd,&value,1)==1);}
static char next(int fd){char value=0;assert(read(fd,&value,1)==1);return value;}
static void exec_again(const char *program,int channel){
 KljProcessExecIdentity state;KljProcessExecLease *leases=NULL;
 assert(!klj_process_exec_snapshot(&state,&leases)&&state.count==1);
 assert(!fcntl(state.directory,F_SETFD,0));assert(!fcntl(leases[0].descriptor,F_SETFD,0));
 char args[7][32];int values[]={state.directory,state.leader,state.parent,state.host_parent,leases[0].descriptor,channel};
 for(int i=0;i<6;i++)snprintf(args[i],sizeof args[i],"%d",values[i]);
 execl(program,program,"--exec",args[0],args[1],args[2],args[3],args[4],args[5],NULL);_exit(99);
}
int main(int argc,char **argv){
 alarm(20);
 if(argc==8&&!strcmp(argv[1],"--exec")){
  KljProcessExecIdentity state={atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),1};
  KljProcessExecLease lease={state.leader,atoi(argv[6]),0};
  assert(!klj_process_exec_adopt(&state,&lease));
  assert(klj_process_prctl(PR_GET_DUMPABLE,0,0,0,0)==1);
  byte(atoi(argv[7]),'x');_exit(0);
 }
 assert(argc==2&&klj_process_ids_configure(argv[1])>0);
 assert(!klj_process_publish_principal(10023));
 assert(klj_process_prctl(PR_GET_DUMPABLE,0,0,0,0)==1);
 errno=0;assert(klj_process_prctl(PR_SET_DUMPABLE,2,0,0,0)==-1&&errno==EINVAL);
 errno=0;assert(klj_process_prctl(PR_SET_DUMPABLE,UINTPTR_MAX,0,0,0)==-1&&errno==EINVAL);
 assert(klj_process_prctl(PR_GET_DUMPABLE,0,0,0,0)==1);
 assert(!klj_process_prctl(PR_SET_DUMPABLE,0,0,0,0));
 char payload[]="parent-data",output[sizeof payload]={0};
 struct iovec input={payload,sizeof payload},destination={output,sizeof output};
 assert(klj_process_vm_readv(klj_process_id(),&destination,1,&input,1,0)==sizeof payload);
 assert(!strcmp(payload,output));
 pid_t inherited=klj_process_fork();assert(inherited>=0);
 if(!inherited)_exit(klj_process_get_dumpable()==0?0:91);
 int status;assert(klj_process_wait_native(inherited,&status,0,NULL)==inherited&&WIFEXITED(status)&&!WEXITSTATUS(status));
 assert(!klj_process_set_dumpable(1));
 int messages[2],commands[2];assert(!pipe(messages)&&!pipe(commands));
 pid_t child=klj_process_fork();assert(child>=0);
 if(!child){
  close(messages[0]);close(commands[1]);memcpy(payload,"child--data",sizeof payload);
  assert(klj_process_get_dumpable()==1);byte(messages[1],'r');
  assert(next(commands[0])=='d');assert(!klj_process_prctl(PR_SET_DUMPABLE,0,0,0,0));
  assert(klj_process_vm_readv(klj_process_id(),&destination,1,&input,1,0)==sizeof payload);
  byte(messages[1],'d');assert(next(commands[0])=='e');assert(!klj_process_set_dumpable(1));byte(messages[1],'e');
  assert(next(commands[0])=='x');assert(!klj_process_set_dumpable(0));exec_again(argv[0],messages[1]);
 }
 close(messages[1]);close(commands[0]);assert(next(messages[0])=='r');
 assert(!klj_process_may_inspect(child));
 errno=0;ssize_t amount=klj_process_vm_readv(child,&destination,1,&input,1,0);
 // macOS may independently deny task_for_pid, even for an enrolled child.
 // Never weaken that policy to make a Linux inspection test succeed.
 assert(amount==(ssize_t)sizeof payload||(amount==-1&&errno==EPERM));
 if(amount>0)assert(!strcmp(output,"child--data"));
 byte(commands[1],'d');assert(next(messages[0])=='d');assert(klj_process_dumpable_of(child)==0);
 errno=0;assert(klj_process_vm_readv(child,&destination,1,&input,1,0)==-1&&errno==EPERM);
 errno=0;assert(klj_process_vm_writev(child,&destination,1,&input,1,0)==-1&&errno==EPERM);
 byte(commands[1],'e');assert(next(messages[0])=='e');
 assert(!klj_process_may_inspect(child));
 if(amount>0)assert(klj_process_vm_readv(child,&destination,1,&input,1,0)==sizeof payload&&!strcmp(output,"child--data"));
 byte(commands[1],'x');assert(next(messages[0])=='x');
 assert(klj_process_wait_native(child,&status,0,NULL)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 close(messages[0]);close(commands[1]);
 puts("PASS guest dumpability defaults, invalid values, self-access, fork inheritance, remote read/write policy denial with host restrictions retained and successful exec reset");
}
