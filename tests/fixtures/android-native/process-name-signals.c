#include "process-identity.h"
#include "process-ids.h"
#include "thread-identity.h"
#include "signal-bridge.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>

static _Atomic int worker_id,delivered,finish;
static void received(int signal){assert(signal==SIGUSR1);atomic_store(&delivered,1);}
static void *worker(void *unused){
    (void)unused;atomic_store(&worker_id,klj_thread_id());
    while(!atomic_load(&finish))usleep(1000);
    return NULL;
}
int main(int argc,char **argv){
    if(argc==7&&!strcmp(argv[1],"--exec")){
        KljProcessExecIdentity identity={atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),1};
        KljProcessExecLease lease={identity.leader,atoi(argv[6]),0};
        assert(!klj_process_exec_adopt(&identity,&lease));
        size_t length;char *command=klj_process_commandline(&length);
        assert(command&&length==sizeof("com.example.game:child")&&!memcmp(command,"com.example.game:child",length));free(command);
        assert(!klj_process_name("com.example.game:exec"));
        char physical[PATH_MAX],bytes[128];
        assert(klj_process_cmdline("/proc/self/cmdline",physical,sizeof physical)==1);
        int fd=open(physical,O_RDONLY);assert(fd>=0);
        ssize_t size=read(fd,bytes,sizeof bytes);assert(!close(fd));
        assert(size==(ssize_t)sizeof("com.example.game:exec")&&!memcmp(bytes,"com.example.game:exec",(size_t)size));
        return 0;
    }
    assert(argc==2&&klj_process_ids_configure(argv[1])>0);
    int process=klj_process_id();
    const char **name=klj_process_identity_symbol("__progname");
    const char *(*get_name)(void)=klj_process_identity_symbol("getprogname");
    assert(name&&get_name&&!*get_name());
    assert(klj_process_name("/android/com.example.game")==0);
    assert(!strcmp(*name,"com.example.game")&&get_name()==*name);
    char physical[PATH_MAX],numeric[96],bytes[256];
    assert(klj_process_cmdline("/proc/self/cmdline",physical,sizeof physical)==1);
    struct stat metadata;assert(!stat(physical,&metadata)&&(metadata.st_mode&0222)==0);
    int snapshot=open(physical,O_RDONLY);assert(snapshot>=0);
    ssize_t size=read(snapshot,bytes,sizeof bytes);
    assert(size==(ssize_t)sizeof("/android/com.example.game")&&!memcmp(bytes,"/android/com.example.game",(size_t)size));
    errno=0;int writable=open(physical,O_WRONLY);assert(writable<0&&errno==EACCES);
    assert(klj_process_name("com.example.game:service")==0);
    snprintf(numeric,sizeof numeric,"/proc/%d/cmdline",process);
    assert(klj_process_cmdline(numeric,physical,sizeof physical)==1);
    int current=open(physical,O_RDONLY);assert(current>=0);
    size=read(current,bytes,sizeof bytes);assert(!close(current));
    assert(size==(ssize_t)sizeof("com.example.game:service")&&!memcmp(bytes,"com.example.game:service",(size_t)size));
    assert(lseek(snapshot,0,SEEK_SET)==0);
    size=read(snapshot,bytes,sizeof bytes);assert(!close(snapshot));
    assert(size==(ssize_t)sizeof("/android/com.example.game")&&!memcmp(bytes,"/android/com.example.game",(size_t)size));
    snprintf(numeric,sizeof numeric,"/proc/%d/cmdline",process+1);
    assert(klj_process_cmdline(numeric,physical,sizeof physical)==0);
    for(const char **path=(const char *[]){"/proc/0/cmdline","/proc/self/cmdline-extra","/proc/self/task/1/cmdline","/proc/18446744073709551616/cmdline",NULL};*path;path++)
        assert(klj_process_cmdline(*path,physical,sizeof physical)==0);
    errno=0;assert(klj_process_cmdline("/proc/self/cmdline",physical,2)==-1&&errno==ENAMETOOLONG);

    assert(!strcmp(get_name(),"com.example.game:service"));
    int channel[2];assert(!pipe(channel));
    pid_t child=klj_process_fork();assert(child>=0);
    if(!child){
        close(channel[0]);
        assert(klj_process_cmdline("/proc/self/cmdline",physical,sizeof physical)==1);
        current=open(physical,O_RDONLY);assert(current>=0);
        size=read(current,bytes,sizeof bytes);assert(!close(current));
        assert(size==(ssize_t)sizeof("com.example.game:service")&&!memcmp(bytes,"com.example.game:service",(size_t)size));
        assert(!klj_process_name("com.example.game:child"));
        assert(klj_process_cmdline("/proc/self/cmdline",physical,sizeof physical)==1);
        current=open(physical,O_RDONLY);assert(current>=0);
        size=read(current,bytes,sizeof bytes);assert(!close(current));
        assert(size==(ssize_t)sizeof("com.example.game:child")&&!memcmp(bytes,"com.example.game:child",(size_t)size));
        assert(write(channel[1],"1",1)==1);close(channel[1]);
        KljProcessExecIdentity identity;KljProcessExecLease *leases;
        assert(!klj_process_exec_snapshot(&identity,&leases)&&identity.count==1);
        assert(!fcntl(identity.directory,F_SETFD,0)&&!fcntl(leases[0].descriptor,F_SETFD,0));
        char directory[32],leader[32],parent[32],host_parent[32],descriptor[32];
        snprintf(directory,sizeof directory,"%d",identity.directory);snprintf(leader,sizeof leader,"%d",identity.leader);
        snprintf(parent,sizeof parent,"%d",identity.parent);snprintf(host_parent,sizeof host_parent,"%d",identity.host_parent);
        snprintf(descriptor,sizeof descriptor,"%d",leases[0].descriptor);free(leases);
        execl(argv[0],argv[0],"--exec",directory,leader,parent,host_parent,descriptor,(char*)NULL);_exit(127);
    }
    close(channel[1]);assert(read(channel[0],bytes,1)==1);close(channel[0]);
    int status;assert(klj_process_wait_native(child,&status,0,NULL)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    assert(klj_process_cmdline("/proc/self/cmdline",physical,sizeof physical)==1);
    current=open(physical,O_RDONLY);assert(current>=0);
    size=read(current,bytes,sizeof bytes);assert(!close(current));
    assert(size==(ssize_t)sizeof("com.example.game:service")&&!memcmp(bytes,"com.example.game:service",(size_t)size));
    assert(!strcmp(get_name(),"com.example.game:service"));

    assert(klj_thread_id()==process);
    assert(!klj_signal_thread_id(process,process,0));
    struct sigaction action={0};action.sa_handler=received;sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1,&action,NULL));
    pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
    for(unsigned i=0;!atomic_load(&worker_id)&&i<1000;i++)usleep(1000);
    int id=atomic_load(&worker_id);assert(id>0);
    int (*tgkill)(int,int,int)=klj_signal_lookup("tgkill");assert(tgkill);
    assert(!tgkill(process,id,10));
    for(unsigned i=0;!atomic_load(&delivered)&&i<1000;i++)usleep(1000);
    assert(atomic_load(&delivered));
    errno=0;assert(tgkill(process+1,id,0)==-1&&errno==ESRCH);
    errno=0;assert(tgkill(process,id,65)==-1&&errno==EINVAL);
    atomic_store(&finish,1);assert(!pthread_join(thread,NULL));
    errno=0;assert(tgkill(process,id,0)==-1&&errno==ESRCH);
    puts("PASS Android process-name data, cmdline snapshots and live-thread signal ownership");
}
