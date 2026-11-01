#include "process-ids.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/xattr.h>
#include <arpa/inet.h>
#include <poll.h>
#include <dirent.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stddef.h>
#include <stdarg.h>

static _Atomic(const KljProcessDescriptors *) descriptor_owner;
int klj_process_descriptors_owned(const KljProcessDescriptors *value){
    if(!value||!value->enter||!value->adopt||!value->closed||!value->leave){errno=EINVAL;return -1;}
    const KljProcessDescriptors *before=NULL;
    if(atomic_compare_exchange_strong(&descriptor_owner,&before,value)||before==value)return 0;
    errno=EALREADY;return -1;
}
static int adopt_descriptor(const KljProcessDescriptors *owner,int fd){
    if(fd<0||!owner)return fd;
    int adopted=owner->adopt(fd);if(adopted>=0)return adopted;
    int error=errno;
    if(error==EBADF)fprintf(stderr,"[android-host] Process descriptor failed stage=adopt fd=%d native_flags=%d\n",fd,fcntl(fd,F_GETFD));
    errno=error;return -1;
}
int klj_process_descriptor_duplicate(int descriptor){
    const KljProcessDescriptors *owner=atomic_load(&descriptor_owner);void *scope=owner?owner->enter():NULL;
    int fd=adopt_descriptor(owner,fcntl(descriptor,F_DUPFD_CLOEXEC,0)),error=errno;
    if(owner)owner->leave(scope);errno=error;return fd;
}
static int owned_open(const char *path,int flags,...){
    mode_t mode=0;if(flags&O_CREAT){va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
    const KljProcessDescriptors *owner=atomic_load(&descriptor_owner);void *scope=owner?owner->enter():NULL;
    int fd=adopt_descriptor(owner,open(path,flags,mode)),error=errno;
    if(owner)owner->leave(scope);errno=error;return fd;
}
static int owned_openat(int directory,const char *path,int flags,...){
    mode_t mode=0;if(flags&O_CREAT){va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
    const KljProcessDescriptors *owner=atomic_load(&descriptor_owner);void *scope=owner?owner->enter():NULL;
    int raw=openat(directory,path,flags,mode),error=errno;
    if(raw<0&&error==EBADF)fprintf(stderr,"[android-host] Process descriptor failed stage=openat directory=%d native_flags=%d\n",directory,fcntl(directory,F_GETFD));
    errno=error;int fd=adopt_descriptor(owner,raw);error=errno;
    if(owner)owner->leave(scope);errno=error;return fd;
}
static int owned_close(int fd){
    const KljProcessDescriptors *owner=atomic_load(&descriptor_owner);void *scope=owner?owner->enter():NULL;
    int result=close(fd),error=errno;if(owner){if(!result)owner->closed(fd);owner->leave(scope);}errno=error;return result;
}
static int owned_closedir(DIR *directory){
    int fd=dirfd(directory);const KljProcessDescriptors *owner=atomic_load(&descriptor_owner);void *scope=owner?owner->enter():NULL;
    int result=closedir(directory),error=errno;if(owner){if(!result)owner->closed(fd);owner->leave(scope);}errno=error;return result;
}
static int owned_kqueue(void){
    const KljProcessDescriptors *owner=atomic_load(&descriptor_owner);void *scope=owner?owner->enter():NULL;
    int fd=adopt_descriptor(owner,kqueue()),error=errno;if(owner)owner->leave(scope);errno=error;return fd;
}
static int owned_socketpair(int domain,int type,int protocol,int pair[2]){
    const KljProcessDescriptors *owner=atomic_load(&descriptor_owner);void *scope=owner?owner->enter():NULL;
    int result=socketpair(domain,type,protocol,pair),error=errno;
    if(!result&&owner){
        int left=adopt_descriptor(owner,pair[0]);
        if(left<0){error=errno;close(pair[1]);result=-1;}
        else{
            int right=adopt_descriptor(owner,pair[1]);
            if(right<0){error=errno;close(left);owner->closed(left);result=-1;}
            else {pair[0]=left;pair[1]=right;}
        }
    }
    if(owner)owner->leave(scope);errno=error;return result;
}
#define open owned_open
#define openat owned_openat
#define close owned_close
#define closedir owned_closedir
#define kqueue owned_kqueue
#define socketpair owned_socketpair

typedef struct {
    uint32_t version,kind;
    int32_t host_pid,process_id,group,session;
    uint32_t group_leader,session_leader;
    int32_t uid,death_signal;
} Lease;
typedef struct { int descriptor; pid_t child; } Slot;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local sigset_t identity_mask;
static void lock_ids(void){
    sigset_t all;sigfillset(&all);pthread_sigmask(SIG_BLOCK,&all,&identity_mask);
    pthread_mutex_lock(&mutex);
}
static void unlock_ids(void){
    pthread_mutex_unlock(&mutex);pthread_sigmask(SIG_SETMASK,&identity_mask,NULL);
}
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int directory_fd=-1,initialization_error;
static _Atomic int process_query_fd=-1;
static pthread_mutex_t process_query_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Atomic int leader_id,parent_id;
static _Atomic int principal_uid=-1,leader_descriptor=-1;
static _Atomic int dumpable=1;
static pid_t parent_host;
static int inherited_group,inherited_session;
/* fd+1 distinguishes a descriptor numbered zero from an unused ID. This is
 * the ownership of open kernel leases, not a second process catalog. */
static Slot leases[KLJ_PROCESS_ID_MAX+1];
static _Atomic pid_t child_hosts[KLJ_PROCESS_ID_MAX+1];
static _Atomic int child_descriptors[KLJ_PROCESS_ID_MAX+1];
static unsigned cursor=KLJ_PROCESS_ID_MIN;
static int fork_reservation;
static _Thread_local int planned_fork;
static const char security_attribute[]="com.maclauncher.android.execution-context";
static const char command_attribute[]="com.maclauncher.android.process-command";
static char *current_command;
static const char dumpable_attribute[]="com.maclauncher.android.dumpable";
typedef struct {
    uint32_t version;int32_t host_pid,process_id,state;
    uint64_t start_seconds,start_microseconds;
} Dumpability;
static int process_birth(pid_t host,uint64_t *seconds,uint64_t *microseconds){
    struct proc_bsdinfo info={0};errno=0;
    int size=proc_pidinfo(host,PROC_PIDTBSDINFO,1,&info,sizeof info);
    if(size!=sizeof info||info.pbi_pid!=(uint32_t)host){if(!errno)errno=ESRCH;return -1;}
    *seconds=info.pbi_start_tvsec;*microseconds=info.pbi_start_tvusec;return 0;
}
static int publish_dumpable(int fd,int process,int value){
    Dumpability policy={.version=1,.host_pid=getpid(),.process_id=process,.state=value};
    if(process_birth(getpid(),&policy.start_seconds,&policy.start_microseconds))return -1;
    return fsetxattr(fd,dumpable_attribute,&policy,sizeof policy,0,0);
}
static _Atomic(const KljProcessForkTransport *) fork_transport;
int klj_process_fork_transport(const KljProcessForkTransport *value){
    if(!value||!value->prepare||!value->child){errno=EINVAL;return -1;}
    const KljProcessForkTransport *before=NULL;
    if(atomic_compare_exchange_strong(&fork_transport,&before,value)||before==value)return 0;
    errno=EALREADY;return -1;
}

static int private_directory(const char *path){
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat status;
    if(fstat(fd,&status)){int error=errno;close(fd);errno=error;return -1;}
    if(!S_ISDIR(status.st_mode)||status.st_uid!=getuid()||(status.st_mode&077)){
        close(fd);errno=EACCES;return -1;
    }
    return fd;
}
static int same_directory(int left,int right){
    struct stat a,b;
    return !fstat(left,&a)&&!fstat(right,&b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino;
}
static int set_lock(int fd,short type,off_t start,off_t length){
    struct flock lock={.l_type=type,.l_whence=SEEK_SET,.l_start=start,.l_len=length};
    return fcntl(fd,F_SETLK,&lock);
}
static int lock_process_state(int fd){
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=2,.l_len=1};
    return fcntl(fd,F_SETLKW,&lock);
}
static int read_record(int fd,Lease *record){
    struct stat status;
    return !fstat(fd,&status)&&S_ISREG(status.st_mode)&&status.st_nlink==1&&
        status.st_uid==getuid()&&!(status.st_mode&077)&&
        pread(fd,record,sizeof(*record),0)==(ssize_t)sizeof(*record)&&
        record->version==3&&(record->kind==1||record->kind==2)&&
        record->process_id>=KLJ_PROCESS_ID_MIN&&record->process_id<=KLJ_PROCESS_ID_MAX;
}
static int write_record(int fd,const Lease *record){
    if(pwrite(fd,record,sizeof(*record),0)!=(ssize_t)sizeof(*record)){
        if(!errno)errno=EIO;return -1;
    }
    return ftruncate(fd,sizeof(*record));
}
/* Darwin also retains process-group/session identities after the leader exits.
 * A released Android leader slot must not be reused while those kernel owners
 * still exist. This runs only when allocating an otherwise unlocked old slot,
 * never on getpid/gettid or a live-process lookup. */
static int retained_kernel_group(const Lease *record){
    if(record->group_leader&&(!kill(-record->host_pid,0)||errno==EPERM))return 1;
    if(!record->session_leader)return 0;
    int count=proc_listallpids(NULL,0);
    if(count<=0)return 1; // Do not recycle an identity after a failed snapshot.
    size_t size=((size_t)count+32)*sizeof(pid_t);
    pid_t *pids=malloc(size);if(!pids)return 1;
    count=proc_listallpids(pids,(int)size);int retained=count<0;
    for(int i=0;i<count&&!retained;i++)if(pids[i]>0&&getsid(pids[i])==record->host_pid)retained=1;
    free(pids);return retained;
}
static int open_id_locked(int id){
    if(id<KLJ_PROCESS_ID_MIN||id>KLJ_PROCESS_ID_MAX){errno=ESRCH;return -1;}
    char name[16];snprintf(name,sizeof name,"%d",id);
    return openat(directory_fd,name,O_RDWR|O_NOFOLLOW|O_CLOEXEC);
}
/* Reverse lookup names select the existing lease; they carry no identity or
 * lifetime of their own. A symlink avoids opening another descriptor for an
 * inode whose POSIX locks this process may own. Every lookup still checks the
 * original record and its live kernel lock. */
static int host_index_locked(int create){
    if(create&&mkdirat(directory_fd,"by-host",0700)&&errno!=EEXIST)return -1;
    int fd=openat(directory_fd,"by-host",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat status;
    if(fstat(fd,&status)||!S_ISDIR(status.st_mode)||status.st_uid!=getuid()||(status.st_mode&077)){
        close(fd);errno=EACCES;return -1;
    }
    return fd;
}
static int indexed_id_locked(int directory,pid_t host){
    char name[32],target[16];snprintf(name,sizeof name,"%d",host);
    ssize_t size=readlinkat(directory,name,target,sizeof(target));
    if(size<=3||size>=(ssize_t)sizeof(target)||memcmp(target,"../",3))return -1;
    unsigned id=0;
    for(ssize_t i=3;i<size;i++){if(target[i]<'0'||target[i]>'9')return -1;id=id*10u+(unsigned)(target[i]-'0');if(id>KLJ_PROCESS_ID_MAX)return -1;}
    return id>=KLJ_PROCESS_ID_MIN?(int)id:-1;
}
static void index_process_locked(int id,pid_t host){
    int saved=errno,directory=host_index_locked(1);
    if(directory>=0){
        char name[32],target[16],temporary[64];
        snprintf(name,sizeof name,"%d",host);snprintf(target,sizeof target,"../%d",id);
        snprintf(temporary,sizeof temporary,".%d-%d",getpid(),id);
        (void)unlinkat(directory,temporary,0);
        if(!symlinkat(target,directory,temporary)){
            (void)renameat(directory,temporary,directory,name);
            (void)unlinkat(directory,temporary,0);
        }
        close(directory);
    }
    errno=saved;
}
static void forget_index_locked(int id,const Lease *prior){
    if(prior->kind!=1||prior->host_pid<=0)return;
    int saved=errno,directory=host_index_locked(0);
    if(directory>=0){
        if(indexed_id_locked(directory,prior->host_pid)==id){char name[32];snprintf(name,sizeof name,"%d",prior->host_pid);(void)unlinkat(directory,name,0);}
        close(directory);
    }
    errno=saved;
}
static void release_child_locked(int id){
    atomic_store_explicit(&child_hosts[id],0,memory_order_release);
    atomic_store_explicit(&child_descriptors[id],0,memory_order_release);
    close(leases[id].descriptor-1);leases[id]=(Slot){0};
    if((unsigned)id<cursor)cursor=(unsigned)id;
}
static int acquire_locked(int kind){
    if(directory_fd<0){errno=ENXIO;return -1;}
    // SA_NOCLDWAIT/SIG_IGN can remove a native zombie without wait. Detect that
    // kernel state while allocating, leaving every observable zombie leased.
    for(int id=KLJ_PROCESS_ID_MIN;id<=KLJ_PROCESS_ID_MAX;id++)if(leases[id].child){
        siginfo_t state={0};
        if(waitid(P_PID,(id_t)leases[id].child,&state,WEXITED|WNOHANG|WNOWAIT)<0&&errno==ECHILD)
            release_child_locked(id);
    }
    const unsigned first=cursor;
    do{
        unsigned id=cursor++;
        if(cursor>KLJ_PROCESS_ID_MAX)cursor=KLJ_PROCESS_ID_MIN;
        // Any close of this inode would drop every POSIX lock owned here.
        if(leases[id].descriptor)continue;
        char name[16];snprintf(name,sizeof name,"%u",id);
        int fd=openat(directory_fd,name,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0)return -1;
        struct stat status;
        if(fstat(fd,&status)||!S_ISREG(status.st_mode)||status.st_nlink!=1||
            status.st_uid!=getuid()||(status.st_mode&077)){
            close(fd);errno=EACCES;return -1;
        }
        if(set_lock(fd,F_WRLCK,0,0)){
            int error=errno;close(fd);
            if(error==EACCES||error==EAGAIN)continue;
            errno=error;return -1;
        }
        Lease prior;
        if(read_record(fd,&prior)){
            if(retained_kernel_group(&prior)){close(fd);continue;}
            // Numeric lease inodes stay stable to prevent lock/inode ABA.
            // Reusing a retired slot also retires its old reverse name, so
            // historical physical PIDs do not accumulate an unbounded index.
            forget_index_locked((int)id,&prior);
        }
        Lease record={.version=3,.kind=(uint32_t)kind,.host_pid=getpid(),
            .process_id=kind==1?(int32_t)id:leader_id,.uid=atomic_load(&principal_uid)};
        if(kind==1){
            record.group=inherited_group;record.session=inherited_session;
            if(getpgrp()==getpid())record.group=(int32_t)id,record.group_leader=1;
            if(getsid(0)==getpid())record.session=(int32_t)id,record.session_leader=1;
        }
        if((fremovexattr(fd,security_attribute,0)&&errno!=ENOATTR)||
           (fremovexattr(fd,command_attribute,0)&&errno!=ENOATTR)||
           publish_dumpable(fd,record.process_id,atomic_load(&dumpable))||write_record(fd,&record)||set_lock(fd,F_UNLCK,1,0)){
            int error=errno;close(fd);errno=error;return -1;
        }
        // Byte zero authenticates the live process. A fork parent owns byte
        // one separately until wait observes/reaps its child.
        leases[id]=(Slot){fd+1,0};return (int)id;
    }while(cursor!=first);
    errno=EAGAIN;return -1;
}
static void before_fork(void){lock_ids();}
static void after_parent(void){unlock_ids();}
static void after_child(void){
    /* POSIX locks do not survive fork. Close copied descriptors without
     * touching the parent's leases; the child's first query claims its own. */
    Lease original;
    if(leader_id&&read_record(leases[leader_id].descriptor-1,&original)){
        inherited_group=original.group;inherited_session=original.session;
    }
    for(unsigned i=KLJ_PROCESS_ID_MIN;i<=KLJ_PROCESS_ID_MAX;i++)if(leases[i].descriptor){
        close(leases[i].descriptor-1);leases[i]=(Slot){0};atomic_store(&child_hosts[i],0);atomic_store(&child_descriptors[i],0);
    }
    parent_id=leader_id;parent_host=getppid();leader_id=0;leader_descriptor=-1;fork_reservation=planned_fork;
    // A stream is serialized per physical owner. A native fork must not mix
    // replies with its parent; explicitly spawned owners receive a fresh one.
    if(process_query_fd>=0){close(process_query_fd);process_query_fd=-1;}
    pthread_mutex_t query_fresh=PTHREAD_MUTEX_INITIALIZER;memcpy(&process_query_mutex,&query_fresh,sizeof query_fresh);
    pthread_mutex_t fresh=PTHREAD_MUTEX_INITIALIZER;memcpy(&mutex,&fresh,sizeof mutex);
    pthread_sigmask(SIG_SETMASK,&identity_mask,NULL);
}
static void initialize(void){
    // Kernel locks already separate concurrent owners. Starting at the first
    // slot reuses crashed owners' inodes instead of populating the ID range.
    cursor=KLJ_PROCESS_ID_MIN;
    initialization_error=pthread_atfork(before_fork,after_parent,after_child);
    const char *text=getenv("KL_ANDROID_PROCESS_QUERY_FD");
    if(text&&*text){
        char *end;long descriptor=strtol(text,&end,10);
        int type=0;socklen_t size=sizeof type;struct sockaddr_storage address;socklen_t extent=sizeof address;
        if(!*end&&descriptor>=0&&descriptor<=INT32_MAX&&
           !getsockopt((int)descriptor,SOL_SOCKET,SO_TYPE,&type,&size)&&type==SOCK_STREAM&&
           !getsockname((int)descriptor,(struct sockaddr*)&address,&extent)&&address.ss_family==AF_UNIX){
            process_query_fd=klj_process_descriptor_duplicate((int)descriptor);
            if(process_query_fd>=0){
                int one=1;
                (void)setsockopt(process_query_fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one);
            }
        }
    }
}
int klj_process_ids_configure(const char *path){
    int fd=private_directory(path);if(fd<0)return -1;
    pthread_once(&once,initialize);
    if(initialization_error){close(fd);errno=initialization_error;return -1;}
    lock_ids();
    if(directory_fd>=0){
        if(!same_directory(fd,directory_fd)){unlock_ids();close(fd);errno=EPERM;return -1;}
        close(fd);
    }else directory_fd=fd;
    if(!leader_id){int id=acquire_locked(1);if(id>0){leader_id=id;leader_descriptor=leases[id].descriptor-1;}}
    if(leader_id)index_process_locked(leader_id,getpid());
    int result=leader_id?leader_id:-1;
    unlock_ids();return result;
}
int klj_process_id(void){
    int current=atomic_load_explicit(&leader_id,memory_order_acquire);
    if(current)return current;
    lock_ids();
    if(!leader_id){int id=acquire_locked(1);if(id>0){leader_id=id;leader_descriptor=leases[id].descriptor-1;index_process_locked(id,getpid());}}
    int result=leader_id?leader_id:-1;
    unlock_ids();return result;
}
int klj_process_parent_id(void){return getppid()==parent_host?atomic_load_explicit(&parent_id,memory_order_acquire):0;}
int klj_process_exec_snapshot(KljProcessExecIdentity *identity,KljProcessExecLease **result){
    if(!identity||!result){errno=EFAULT;return -1;}
    if(klj_process_id()<0)return -1;
    lock_ids();size_t count=1;
    for(unsigned id=KLJ_PROCESS_ID_MIN;id<=KLJ_PROCESS_ID_MAX;id++)if(leases[id].child)count++;
    KljProcessExecLease *items=calloc(count,sizeof *items);
    if(!items){unlock_ids();return -1;}
    size_t at=0;items[at++]=(KljProcessExecLease){leader_id,leader_descriptor,0};
    for(unsigned id=KLJ_PROCESS_ID_MIN;id<=KLJ_PROCESS_ID_MAX;id++)if(leases[id].child)
        items[at++]=(KljProcessExecLease){(int)id,leases[id].descriptor-1,leases[id].child};
    *identity=(KljProcessExecIdentity){directory_fd,leader_id,parent_id,parent_host,(uint32_t)count};
    *result=items;unlock_ids();return 0;
}
int klj_process_exec_adopt(const KljProcessExecIdentity *identity,const KljProcessExecLease *items){
    if(!identity||!items||!identity->count||identity->count>KLJ_PROCESS_ID_MAX||
       identity->leader<KLJ_PROCESS_ID_MIN||identity->leader>KLJ_PROCESS_ID_MAX){errno=EINVAL;return -1;}
    pthread_once(&once,initialize);
    if(initialization_error){errno=initialization_error;return -1;}
    struct stat directory;
    if(fstat(identity->directory,&directory)||!S_ISDIR(directory.st_mode)||directory.st_uid!=getuid()||(directory.st_mode&077)){errno=EACCES;return -1;}
    lock_ids();
    if(directory_fd>=0||leader_id){unlock_ids();errno=EALREADY;return -1;}
    int error=0;Lease leader={0};unsigned char seen[KLJ_PROCESS_ID_MAX+1]={0};
    for(uint32_t i=0;i<identity->count&&!error;i++){
        const KljProcessExecLease *item=items+i;Lease record;struct stat fd,path;
        char name[16];snprintf(name,sizeof name,"%d",item->id);
        if(item->id<KLJ_PROCESS_ID_MIN||item->id>KLJ_PROCESS_ID_MAX||seen[item->id]||item->descriptor<0||
           fstat(item->descriptor,&fd)||fstatat(identity->directory,name,&path,AT_SYMLINK_NOFOLLOW)||
           fd.st_dev!=path.st_dev||fd.st_ino!=path.st_ino||!read_record(item->descriptor,&record)||
           record.kind!=1||record.process_id!=item->id){error=EINVAL;break;}
        seen[item->id]=1;
        if(item->id==identity->leader){
            if(item->child||record.host_pid!=getpid()||set_lock(item->descriptor,F_WRLCK,0,1)){error=EPERM;break;}
            leader=record;
        }else if(item->child<=0||record.host_pid!=item->child||set_lock(item->descriptor,F_WRLCK,1,1)){error=EPERM;break;}
    }
    if(!error&&!seen[identity->leader])error=EINVAL;
    // This is reached only after physical exec succeeded. Ordinary installed
    // guest programs have no set-id/file-capability elevation.
    if(!error){
        for(uint32_t i=0;i<identity->count;i++)if(items[i].id==identity->leader)
            if(publish_dumpable(items[i].descriptor,identity->leader,1))error=errno;
    }
    if(!error){
        directory_fd=identity->directory;leader_id=identity->leader;parent_id=identity->parent;parent_host=identity->host_parent;
        principal_uid=leader.uid;dumpable=1;
        for(uint32_t i=0;i<identity->count;i++){
            const KljProcessExecLease *item=items+i;leases[item->id]=(Slot){item->descriptor+1,item->child};
            if(item->id==identity->leader)leader_descriptor=item->descriptor;
            if(item->child){atomic_store(&child_hosts[item->id],item->child);atomic_store(&child_descriptors[item->id],item->descriptor+1);}
        }
        index_process_locked(leader_id,getpid());
    }
    unlock_ids();if(error){errno=error;return -1;}return 0;
}
int klj_thread_id_allocate(void){
    if(klj_process_id()<0)return -1;
    lock_ids();int id=acquire_locked(2);unlock_ids();return id;
}
void klj_thread_id_release(int id){
    lock_ids();
    if(id>=KLJ_PROCESS_ID_MIN&&id<=KLJ_PROCESS_ID_MAX&&id!=leader_id&&
       leases[id].descriptor&&!leases[id].child){
        int fd=leases[id].descriptor-1;leases[id]=(Slot){0};close(fd);
        if((unsigned)id<cursor)cursor=(unsigned)id;
    }
    unlock_ids();
}
static int record_fd_locked(int id,int fd,int owned,Lease *record,int allow_retired){
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=0,.l_len=1};
    int good=read_record(fd,record);
    if(good&&!allow_retired){
        if(owned&&!leases[id].child)good=record->host_pid==getpid();
        else good=!fcntl(fd,F_GETLK,&lock)&&lock.l_type==F_WRLCK&&lock.l_pid==record->host_pid;
    }
    if(!good){errno=ESRCH;return -1;}return 0;
}
static int record_locked(int id,Lease *record,int allow_retired){
    int owned=id>=KLJ_PROCESS_ID_MIN&&id<=KLJ_PROCESS_ID_MAX?leases[id].descriptor:0;
    int fd=owned?owned-1:open_id_locked(id);if(fd<0)return -1;
    int result=record_fd_locked(id,fd,owned,record,allow_retired),error=errno;
    if(!owned)close(fd);
    errno=error;return result;
}
/* The existing kernel lease is also the lifetime of its execution label.
 * One atomic xattr contains the complete thread snapshot; no PID catalog or
 * independently retired security record is introduced. */
int klj_process_publish_security(int id,const void *value,size_t length){
    if(!value||!length){errno=EINVAL;return -1;}
    lock_ids();
    int owned=id>=KLJ_PROCESS_ID_MIN&&id<=KLJ_PROCESS_ID_MAX?leases[id].descriptor:0;
    int result=-1;
    if(!owned||leases[id].child)errno=ESRCH;
    else result=fsetxattr(owned-1,security_attribute,value,length,0,0);
    int error=errno;unlock_ids();errno=error;return result;
}
ssize_t klj_process_read_security(int id,void *value,size_t length){
    lock_ids();Lease before,after={0};
    ssize_t result=-1;
    if(!record_locked(id,&before,0)){
        int owned=leases[id].descriptor,fd=owned?owned-1:open_id_locked(id);
        if(fd>=0){
            result=fgetxattr(fd,security_attribute,value,length,0,0);
            int error=errno;
            if(!read_record(fd,&after)||before.host_pid!=after.host_pid||before.process_id!=after.process_id){result=-1;error=ESRCH;}
            if(!owned){
                struct flock live={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=0,.l_len=1};
                if(fcntl(fd,F_GETLK,&live)||live.l_type!=F_WRLCK||live.l_pid!=after.host_pid){result=-1;error=ESRCH;}
            }
            if(!owned)close(fd);
            errno=error;
        }
    }
    int error=errno;unlock_ids();errno=error;return result;
}
int klj_process_get_dumpable(void){
    if(klj_process_id()<0)return -1;
    return atomic_load_explicit(&dumpable,memory_order_acquire);
}
int klj_process_set_dumpable(int value){
    if(value!=0&&value!=1){errno=EINVAL;return -1;}
    if(klj_process_id()<0)return -1;
    lock_ids();
    int result=publish_dumpable(atomic_load(&leader_descriptor),leader_id,value);
    if(!result)atomic_store_explicit(&dumpable,value,memory_order_release);
    int error=errno;unlock_ids();errno=error;return result;
}
/* A peer's policy is read on its retained, kernel-authenticated leader inode.
 * Old/malformed policy records deny inspection; they never imply permission. */
static int dumpable_locked(int id,Lease *owner){
    Lease before,after;if(record_locked(id,&before,0))return -1;
    if(before.kind==2){id=before.process_id;if(record_locked(id,&before,0))return -1;}
    int owned=leases[id].descriptor,fd=owned?owned-1:open_id_locked(id);
    if(fd<0)return -1;
    Dumpability policy={0};ssize_t count=fgetxattr(fd,dumpable_attribute,&policy,sizeof policy,0,0);
    int error=errno,result=-1;
    uint64_t seconds=0,microseconds=0;
    if(count==sizeof policy&&policy.version==1&&policy.state>=0&&policy.state<=1&&
       policy.host_pid==before.host_pid&&policy.process_id==before.process_id){
        if(process_birth(before.host_pid,&seconds,&microseconds)){error=errno;}
        else if(policy.start_seconds!=seconds||policy.start_microseconds!=microseconds)error=EPERM;
        else if(!record_fd_locked(id,fd,owned,&after,0)&&before.host_pid==after.host_pid&&before.process_id==after.process_id){*owner=after;result=policy.state;}
        else error=ESRCH;
    }else error=count<0?error:EPERM;
    if(!owned)close(fd);errno=error;return result;
}
int klj_process_dumpable_of(int id){
    lock_ids();Lease owner;int result=dumpable_locked(id,&owner),error=errno;
    unlock_ids();errno=error;return result;
}
int klj_process_may_inspect(int id){
    if(klj_process_id()<0)return -1;
    lock_ids();Lease target,caller;int result=-1;
    int state=dumpable_locked(id,&target);
    if(state>=0&&!record_locked(leader_id,&caller,0)){
        if(target.host_pid==getpid()&&target.process_id==leader_id)result=0;
        else if(state==1&&caller.uid>=0&&target.uid==caller.uid)result=0;
        else errno=EPERM;
    }
    int error=errno;unlock_ids();errno=error;return result;
}
int klj_process_publish_principal(uid_t uid){
    if(uid>INT32_MAX){errno=EINVAL;return -1;}
    if(klj_process_id()<0)return -1;
    lock_ids();
    int prior=atomic_load_explicit(&principal_uid,memory_order_acquire);
    if(prior>=0&&prior!=(int)uid){unlock_ids();errno=EPERM;return -1;}
    int fd=atomic_load(&leader_descriptor);int32_t value=(int32_t)uid;
    int result=pwrite(fd,&value,sizeof value,offsetof(Lease,uid))==sizeof value?0:-1;
    if(!result)atomic_store_explicit(&principal_uid,(int)uid,memory_order_release);
    unlock_ids();return result;
}
int klj_process_publish_name(const char *name){
    if(!name){errno=EFAULT;return -1;}
    if(klj_process_id()<0)return -1;
    char *copy=strdup(name);
    lock_ids();char *before=current_command;current_command=copy;
    int result=fsetxattr(atomic_load(&leader_descriptor),command_attribute,name,strlen(name),0,0);
    int error=errno;free(before);unlock_ids();errno=error;return result;
}
char *klj_process_commandline(size_t *length){
    if(!length){errno=EFAULT;return NULL;}*length=0;
    if(klj_process_id()<0)return NULL;
    lock_ids();int fd=atomic_load(&leader_descriptor);
    /* The current name follows fork's copied address space. An exec-adopted
     * owner starts from the metadata on its inherited logical lease instead. */
    ssize_t size=current_command?(ssize_t)strnlen(current_command,1024*1024):fgetxattr(fd,command_attribute,NULL,0,0,0);
    char *result=NULL;int error=errno;
    if(size>=0){
        if(size>=1024*1024)error=E2BIG;
        else{
            result=malloc((size_t)size+1);
            if(!result)error=ENOMEM;
            else{
                ssize_t copied=size;
                if(current_command)memcpy(result,current_command,(size_t)size);
                else copied=fgetxattr(fd,command_attribute,result,(size_t)size,0,0);
                if(copied!=size){error=copied<0?errno:EIO;free(result);result=NULL;}
                else{result[size]=0;*length=(size_t)size+1;}
            }
        }
    }
    unlock_ids();errno=error;return result;
}
static uint64_t query_now(void){
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
    return (uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
}
static int query_transfer(int descriptor,void *bytes,size_t length,int writing,uint64_t deadline){
    size_t at=0;
    while(at<length){
        uint64_t now=query_now();if(!now||now>=deadline){errno=ETIMEDOUT;return -1;}
        ssize_t n=writing?send(descriptor,(char*)bytes+at,length-at,MSG_DONTWAIT):recv(descriptor,(char*)bytes+at,length-at,MSG_DONTWAIT);
        if(n>0){at+=(size_t)n;continue;}
        if(n<0&&errno==EINTR)continue;
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){
            struct pollfd wait={.fd=descriptor,.events=writing?POLLOUT:POLLIN};int result;
            now=query_now();if(!now||now>=deadline){errno=ETIMEDOUT;return -1;}
            int remaining=(int)((deadline-now+999999u)/1000000u);
            result=poll(&wait,1,remaining);if(result<0&&errno==EINTR)continue;
            if(result>0)continue;if(!result)errno=ETIMEDOUT;return -1;
        }
        if(!n)errno=EPIPE;return -1;
    }
    return 0;
}
/* The parent seals the namespace directory on this inherited capability.
 * Only its fresh kernel candidate snapshot crosses the sandbox boundary;
 * names, credentials and guest-selected paths never enter the protocol. */
static int command_hosts(pid_t **hosts,size_t *count){
    if(process_query_fd<0){errno=ENXIO;return -1;}
    sigset_t all,prior;sigfillset(&all);pthread_sigmask(SIG_BLOCK,&all,&prior);
    pthread_mutex_lock(&process_query_mutex);
    unsigned char operation=1;uint32_t size;pid_t *items=NULL;int result=-1;
    if(process_query_fd<0){errno=ENXIO;goto done;}
    uint64_t began=query_now();if(!began)goto done;uint64_t deadline=began+5000000000u;
    if(query_transfer(process_query_fd,&operation,1,1,deadline)||query_transfer(process_query_fd,&size,sizeof size,0,deadline))goto broken;
    uint32_t length=ntohl(size);if(length==UINT32_MAX){errno=EIO;goto done;}
    if(length>1u<<20){errno=EPROTO;goto broken;}
    items=malloc((size_t)length*sizeof *items);if(length&&!items)goto broken;
    if(query_transfer(process_query_fd,items,(size_t)length*sizeof *items,0,deadline))goto broken;
    int self=0;for(uint32_t i=0;i<length;i++){
        uint32_t host=ntohl((uint32_t)items[i]);if(!host||host>INT32_MAX){errno=EPROTO;goto broken;}
        items[i]=(pid_t)host;if(items[i]==getpid())self=1;
    }
    if(!self){errno=EIO;goto done;}
    *hosts=items;*count=length;items=NULL;result=0;goto done;
broken:
    close(process_query_fd);process_query_fd=-1;
done:
    {int error=errno;free(items);pthread_mutex_unlock(&process_query_mutex);pthread_sigmask(SIG_SETMASK,&prior,NULL);errno=error;return result;}
}
static int command_matches(int id,pid_t host,const char *const *names,size_t count){
    lock_ids();Lease before,after;int matched=0;
    if(!record_locked(id,&before,0)&&before.kind==1&&before.process_id==id&&(!host||before.host_pid==host)){
        int owned=leases[id].descriptor,fd=owned?owned-1:open_id_locked(id);
        if(fd>=0){
            char command[PATH_MAX];ssize_t size=fgetxattr(fd,command_attribute,command,sizeof command-1,0,0);
            if(size>=0&&!record_fd_locked(id,fd,owned,&after,0)&&before.host_pid==after.host_pid&&before.process_id==after.process_id){
                command[size]=0;char *space=strchr(command,' ');if(space)*space=0;
                for(size_t i=0;i<count;i++)if(!strcmp(names[i],command)){matched=1;break;}
            }
            if(!owned)close(fd);
        }
    }
    unlock_ids();return matched;
}
static int append_command(int id,int **pids,size_t *length){
    int *next=realloc(*pids,(*length+1)*sizeof **pids);
    if(!next)return -1;*pids=next;(*pids)[(*length)++]=id;return 0;
}
int klj_process_find_commands(const char *const *names,size_t count,int **pids,size_t *length){
    if(!pids||!length||(count&&!names)){errno=EFAULT;return -1;}
    *pids=NULL;*length=0;if(klj_process_id()<0)return -1;
    if(!count)return 0;
    pid_t *hosts=NULL;size_t host_count=0;
    if(!command_hosts(&hosts,&host_count)){
        int result=0;
        for(size_t i=0;i<host_count;i++){
            int id=klj_process_id_from_host(hosts[i]);
            if(id>=0&&command_matches(id,hosts[i],names,count)&&append_command(id,pids,length)){result=-1;break;}
        }
        int error=errno;free(hosts);
        if(result){free(*pids);*pids=NULL;*length=0;}errno=error;return result;
    }
    // A denied or incomplete kernel snapshot must not omit legacy owners.
    // Preserve the original complete lease scan only for that failure path.
    int copy=openat(directory_fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(copy<0)return -1;
    DIR *directory=fdopendir(copy);if(!directory){int error=errno;close(copy);errno=error;return -1;}
    int result=0;struct dirent *entry;
    while((entry=readdir(directory))){
        char *end;long id=strtol(entry->d_name,&end,10);
        if(!*entry->d_name||*end||id<KLJ_PROCESS_ID_MIN||id>KLJ_PROCESS_ID_MAX)continue;
        if(command_matches((int)id,0,names,count)&&append_command((int)id,pids,length)){result=-1;break;}
    }
    int error=errno;closedir(directory);
    if(result){free(*pids);*pids=NULL;*length=0;}errno=error;return result;
}
int klj_process_signal_owner(int id,pid_t *host,int *kind,int *process,uid_t *uid){
    if(!host||!kind||!process||!uid){errno=EFAULT;return -1;}
    lock_ids();Lease record;
    int result=record_locked(id,&record,0);
    if(!result){
        *host=record.host_pid;*kind=(int)record.kind;*process=record.process_id;
        /* Worker leases retain the leader's authenticated principal. */
        if(record.kind==2)result=record_locked(record.process_id,&record,0);
        if(!result&&record.uid<0){errno=EACCES;result=-1;}
        if(!result)*uid=(uid_t)record.uid;
    }
    unlock_ids();return result;
}
static int cpu_usage(pid_t pid,struct rusage_info_v0 *usage){
    if(proc_pid_rusage(pid,RUSAGE_INFO_V0,(rusage_info_t *)usage))return -1;
    /* proc_pid_rusage uses absolute-time ticks; THREAD_EXTENDED_INFO uses ns. */
    mach_timebase_info_data_t base;
    if(mach_timebase_info(&base)!=KERN_SUCCESS||!base.denom){errno=EIO;return -1;}
    usage->ri_user_time=(uint64_t)((__uint128_t)usage->ri_user_time*base.numer/base.denom);
    usage->ri_system_time=(uint64_t)((__uint128_t)usage->ri_system_time*base.numer/base.denom);
    return 0;
}
int klj_process_cpu_time(int id,uint64_t *user,uint64_t *system){
    if(!user||!system){errno=EFAULT;return -1;}
    struct rusage_info_v0 usage={0};
    if(!id){
        if(cpu_usage(getpid(),&usage))return -1;
        *user=usage.ri_user_time;*system=usage.ri_system_time;return 0;
    }
    lock_ids();Lease before,after;int result=-1,fd=-1,owned=0,held=0;
    if(record_locked(id,&before,0)||before.kind!=1){errno=EINVAL;goto done;}
    owned=leases[id].descriptor;fd=owned?owned-1:open_id_locked(id);if(fd<0)goto done;
    // The allocator takes a whole-file write lock before replacing a lease.
    // Retain a read lock on its state byte across sampling and live-owner
    // validation: neither Android ID nor a recycled Mac PID can retarget it.
    struct flock state={.l_type=F_RDLCK,.l_whence=SEEK_SET,.l_start=2,.l_len=1};
    if(fcntl(fd,F_SETLKW,&state))goto done;
    held=1;
    // Reuse this descriptor: closing another alias would drop our POSIX lock.
    if(record_fd_locked(id,fd,owned,&after,0)||after.host_pid!=before.host_pid||after.process_id!=before.process_id){errno=EINVAL;goto done;}
    if(cpu_usage(before.host_pid,&usage))goto done;
    if(record_fd_locked(id,fd,owned,&after,0)||after.host_pid!=before.host_pid||after.process_id!=before.process_id){errno=EINVAL;goto done;}
    *user=usage.ri_user_time;*system=usage.ri_system_time;result=0;
done:;
    int error=errno;if(held)set_lock(fd,F_UNLCK,2,1);if(fd>=0&&!owned)close(fd);
    unlock_ids();errno=error;return result;
}
int klj_process_signal_targets(pid_t selector,int (*send)(int,void *),void *context){
    if(selector>0||selector==INT32_MIN||!send){errno=EINVAL;return -1;}
    int group=selector<-1?-selector:0;
    if(!selector){group=klj_process_getpgid(0);if(group<=0){if(!group)errno=EPERM;return -1;}}
    /* Enumerate the existing leased namespace. There is no separate signal
     * process catalog, and no native launcher/terminal group is selected. */
    int fd=openat(directory_fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -1;
    DIR *directory=fdopendir(fd);if(!directory){int error=errno;close(fd);errno=error;return -1;}
    int delivered=0,error=ESRCH;struct dirent *entry;
    while((entry=readdir(directory))){
        unsigned id=0;const char *at=entry->d_name;
        if(!*at)continue;
        for(;*at>='0'&&*at<='9'&&id<=KLJ_PROCESS_ID_MAX;at++)id=id*10+(unsigned)(*at-'0');
        if(*at||id<KLJ_PROCESS_ID_MIN||id>KLJ_PROCESS_ID_MAX||(selector==-1&&(int)id==klj_process_id()))continue;
        lock_ids();Lease owner;
        int accepted=!record_locked((int)id,&owner,0)&&owner.kind==1&&owner.process_id==(int)id&&(!group||owner.group==group);
        unlock_ids();
        if(!accepted)continue;
        if(!send((int)id,context))delivered=1;else if(errno!=ESRCH)error=errno;
    }
    closedir(directory);if(delivered)return 0;errno=error;return -1;
}
__attribute__((noreturn)) void klj_process_signal_exit(int signal){
    int fd=atomic_load_explicit(&leader_descriptor,memory_order_acquire);
    int32_t value=signal;
    if(fd>=0)(void)pwrite(fd,&value,sizeof value,offsetof(Lease,death_signal));
    kill(getpid(),SIGKILL);_exit(128+signal);
}
static int signal_from_record(int fd,int native_signal){
    int32_t signal=0;
    if(native_signal==SIGKILL&&fd>=0&&pread(fd,&signal,sizeof signal,offsetof(Lease,death_signal))==sizeof signal&&
       signal>0&&signal<=64)return signal;
    return native_signal;
}
int klj_process_child_signal(pid_t host,int native_signal){
    if(native_signal!=SIGKILL)return native_signal;
    int saved_errno=errno;
    for(int id=KLJ_PROCESS_ID_MIN;id<=KLJ_PROCESS_ID_MAX;id++)
        if(atomic_load_explicit(&child_hosts[id],memory_order_acquire)==host){
            int descriptor=atomic_load_explicit(&child_descriptors[id],memory_order_acquire);
            Lease record;
            /* A concurrent wait can retire this lease. Read-only notification
             * decoding validates the complete identity and publication again;
             * it never treats a reused descriptor as the old child's status. */
            int accepted=descriptor>0&&pread(descriptor-1,&record,sizeof record,0)==sizeof record&&
                record.version==3&&record.kind==1&&record.host_pid==host&&record.process_id==id&&
                atomic_load_explicit(&child_descriptors[id],memory_order_acquire)==descriptor&&
                atomic_load_explicit(&child_hosts[id],memory_order_acquire)==host;
            errno=saved_errno;
            return accepted&&record.death_signal>0&&record.death_signal<=64?record.death_signal:native_signal;
        }
    errno=saved_errno;
    return native_signal;
}
int klj_process_id_owner(const char *path,int id,pid_t *host_pid,int *kind,int *process_id){
    if(id<KLJ_PROCESS_ID_MIN||id>KLJ_PROCESS_ID_MAX||!host_pid||!kind||!process_id){errno=EINVAL;return -1;}
    int directory=private_directory(path);if(directory<0)return -1;
    lock_ids();
    // The read-only supervisor helper has no configured namespace. Reuse the
    // same decoder with a borrowed directory; never reopen an owned inode.
    int saved=directory_fd,local=saved>=0&&same_directory(directory,saved);
    if(!local&&saved>=0){unlock_ids();close(directory);errno=EPERM;return -1;}
    if(saved<0)directory_fd=directory;
    Lease record;int result=record_locked(id,&record,0);
    directory_fd=saved;
    if(!result){*host_pid=record.host_pid;*kind=(int)record.kind;*process_id=record.process_id;}
    unlock_ids();close(directory);return result;
}
int klj_process_id_from_host(pid_t host){
    if(host<=0){errno=EINVAL;return -1;}
    if(host==getpid())return klj_process_id();
    lock_ids();
    if(directory_fd<0){unlock_ids();errno=ENXIO;return -1;}
    int indexed=host_index_locked(0);
    if(indexed>=0){
        int id=indexed_id_locked(indexed,host);close(indexed);Lease value;
        if(id>=0&&!record_locked(id,&value,0)&&value.kind==1&&value.process_id==id&&value.host_pid==host){unlock_ids();return id;}
    }
    // Older live owners may have no reverse name. Dead physical processes
    // cannot authenticate, and need no compatibility directory scan.
    if(kill(host,0)<0&&errno==ESRCH){unlock_ids();errno=ESRCH;return -1;}
    int copy=openat(directory_fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(copy<0){unlock_ids();return -1;}
    DIR *directory=fdopendir(copy);
    if(!directory){int error=errno;close(copy);unlock_ids();errno=error;return -1;}
    int result=-1;struct dirent *item;
    while((item=readdir(directory))){
        char *end;long id=strtol(item->d_name,&end,10);
        if(!*item->d_name||*end||id<KLJ_PROCESS_ID_MIN||id>KLJ_PROCESS_ID_MAX)continue;
        Lease value;
        if(!record_locked((int)id,&value,0)&&value.kind==1&&value.process_id==(int)id&&value.host_pid==host){result=(int)id;break;}
    }
    closedir(directory);if(result>=0)index_process_locked(result,host);unlock_ids();if(result<0)errno=ESRCH;return result;
}
int klj_process_id_query(int id,pid_t *host,int *kind,int *process){
    if(!host||!kind||!process){errno=EFAULT;return -1;}
    lock_ids();Lease record;int result=record_locked(id,&record,0);
    if(!result){*host=record.host_pid;*kind=(int)record.kind;*process=record.process_id;}
    int error=errno;unlock_ids();errno=error;return result;
}
int klj_process_child_retain(pid_t host){
    int id=klj_process_id_from_host(host);if(id<0)return -1;
    lock_ids();
    if(leases[id].descriptor){int retained=leases[id].child==host;unlock_ids();if(retained)return id;errno=EALREADY;return -1;}
    int fd=open_id_locked(id),failure=fd<0?errno:0;Lease value;
    if(!failure&&(record_fd_locked(id,fd,0,&value,0)||value.kind!=1||value.host_pid!=host))failure=ESRCH;
    if(!failure&&set_lock(fd,F_WRLCK,1,1))failure=errno;
    if(!failure){
        Lease parent;
        if(!record_locked(leader_id,&parent,0)){
            if(getpgid(host)==getpgrp())value.group=parent.group;
            if(getsid(host)==getsid(0))value.session=parent.session;
            if(lock_process_state(fd))failure=errno;
            else {if(write_record(fd,&value))failure=errno;set_lock(fd,F_UNLCK,2,1);}
        }
    }
    if(!failure){
        leases[id]=(Slot){fd+1,host};
        atomic_store_explicit(&child_hosts[id],host,memory_order_release);
        atomic_store_explicit(&child_descriptors[id],fd+1,memory_order_release);
    }else if(fd>=0)close(fd);
    unlock_ids();if(failure){errno=failure;return -1;}return id;
}
int klj_process_child_reaped(pid_t host){
    lock_ids();int id=-1;
    for(int i=KLJ_PROCESS_ID_MIN;i<=KLJ_PROCESS_ID_MAX;i++)if(leases[i].child==host){id=i;release_child_locked(i);break;}
    unlock_ids();if(id<0)errno=ECHILD;return id;
}
pid_t klj_process_child_host(int id){
    if(id<KLJ_PROCESS_ID_MIN||id>KLJ_PROCESS_ID_MAX)return 0;
    return atomic_load_explicit(&child_hosts[id],memory_order_acquire);
}

static ssize_t transfer(int fd,void *data,size_t size,int send_data){
    size_t done=0;
    while(done<size){
        ssize_t count=send_data?send(fd,(char *)data+done,size-done,0):read(fd,(char *)data+done,size-done);
        if(count<0&&errno==EINTR)continue;
        if(count<=0){if(!count)errno=EPIPE;return -1;}done+=(size_t)count;
    }
    return (ssize_t)done;
}
pid_t klj_process_fork(void){
    if(klj_process_id()<0)return -1;
    const KljProcessForkTransport *transport=atomic_load(&fork_transport);
    int authority=transport?transport->prepare():-1;
    if(transport&&authority<0)return -1;
    int channel[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,channel)){int error=errno;if(authority>=0)close(authority);errno=error;return -1;}
    for(int i=0;i<2;i++){int value=1;fcntl(channel[i],F_SETFD,FD_CLOEXEC);setsockopt(channel[i],SOL_SOCKET,SO_NOSIGPIPE,&value,sizeof value);}
    lock_ids();
    int child_id=acquire_locked(1);Lease parent;
    int failure=child_id<0?errno:record_locked(leader_id,&parent,0)?errno:0;
    if(!failure){
        int fd=leases[child_id].descriptor-1;
        // Claim the parent's reservation before releasing the live byte.
        if(set_lock(fd,F_WRLCK,1,1)||set_lock(fd,F_UNLCK,0,1))failure=errno;
    }
    if(failure&&child_id>0){
        Lease failed={.version=3,.kind=1,.process_id=child_id,.uid=-1};
        (void)write_record(leases[child_id].descriptor-1,&failed);
        close(leases[child_id].descriptor-1);leases[child_id]=(Slot){0};
    }
    unlock_ids();
    if(failure){close(channel[0]);close(channel[1]);if(authority>=0)close(authority);errno=failure;return -1;}
    planned_fork=child_id;
    pid_t child=fork();int fork_error=errno;planned_fork=0;
    if(!child){
        close(channel[0]);
        int result=0,fd=open_id_locked(fork_reservation);
        Lease record={.version=3,.kind=1,.host_pid=getpid(),.process_id=fork_reservation,
            .group=inherited_group,.session=inherited_session,.uid=atomic_load(&principal_uid)};
        if(fd<0||set_lock(fd,F_WRLCK,0,1)||
            publish_dumpable(fd,record.process_id,atomic_load(&dumpable))||write_record(fd,&record))result=errno;
        if(!result){leases[fork_reservation]=(Slot){fd+1,0};leader_id=fork_reservation;leader_descriptor=fd;index_process_locked(leader_id,getpid());}
        // The parent already owns the zombie reservation. Publish the live
        // lease before either side can return from the physical operation.
        int sent=transfer(channel[1],&result,sizeof result,1)==sizeof result;
        char acknowledged=0;
        if(!result&&sent)sent=transfer(channel[1],&acknowledged,1,0)==1;
        close(channel[1]);fork_reservation=0;
        if(result||!sent||!acknowledged)_exit(127);
        if(authority>=0&&transport->child(authority))_exit(127);
        return 0;
    }
    if(authority>=0)close(authority);
    close(channel[1]);
    if(child>0){
        lock_ids();leases[child_id].child=child;
        atomic_store_explicit(&child_hosts[child_id],child,memory_order_release);
        atomic_store_explicit(&child_descriptors[child_id],leases[child_id].descriptor,memory_order_release);
        unlock_ids();
        int error=0;
        if(transfer(channel[0],&error,sizeof error,0)!=sizeof error)error=errno;
        if(!error){char acknowledged=1;if(transfer(channel[0],&acknowledged,1,1)!=1)error=errno;}
        close(channel[0]);
        if(!error)return child_id;
        // A failed unpublished child is still reaped through this reservation.
        kill(child,SIGKILL);while(waitpid(child,NULL,0)<0&&errno==EINTR){}
        fork_error=error;
    }else close(channel[0]);
    lock_ids();
    // No child survived publication; do not retain the creator's group flags
    // from the temporary reservation as a new Android group identity.
    Lease failed={.version=3,.kind=1,.process_id=child_id,.uid=-1};
    (void)write_record(leases[child_id].descriptor-1,&failed);
    release_child_locked(child_id);unlock_ids();
    errno=fork_error;return -1;
}

static int target_locked(pid_t id,pid_t *host,Lease *record){
    if(!id)id=leader_id;
    int child=id>=KLJ_PROCESS_ID_MIN&&id<=KLJ_PROCESS_ID_MAX?leases[id].child:0;
    if(record_locked(id,record,!!child)||record->kind!=1||record->process_id!=id||
       (child&&record->host_pid!=child)){errno=ESRCH;return -1;}
    *host=record->host_pid;return 0;
}
static pid_t group_locked(pid_t id,int session){
    pid_t host;Lease record;if(target_locked(id,&host,&record))return -1;
    pid_t physical=session?getsid(host):getpgid(host);if(physical<0)return -1;
    int result=session?record.session:record.group;
    if(!result)return 0; // Kernel group leader is outside the Android namespace.
    Lease owner;if(record_locked(result,&owner,1))return -1;
    if(owner.host_pid!=physical){errno=ESRCH;return -1;}return result;
}
pid_t klj_process_getpgid(pid_t id){lock_ids();pid_t result=group_locked(id,0);unlock_ids();return result;}
pid_t klj_process_getsid(pid_t id){lock_ids();pid_t result=group_locked(id,1);unlock_ids();return result;}
pid_t klj_process_setsid(void){
    lock_ids();int fd=leader_id?leases[leader_id].descriptor-1:-1;
    if(fd<0||lock_process_state(fd)){unlock_ids();if(fd<0)errno=ESRCH;return -1;}
    pid_t result=setsid();
    if(result>=0){Lease record;if(record_locked(leader_id,&record,0))result=-1;else{
        record.group=record.session=leader_id;record.group_leader=record.session_leader=1;
        result=write_record(fd,&record)?-1:leader_id;
    }}
    int error=errno;(void)set_lock(fd,F_UNLCK,2,1);errno=error;
    unlock_ids();return result;
}
int klj_process_setpgid(pid_t id,pid_t group){
    if(id<0||group<0){errno=EINVAL;return -1;}
    lock_ids();if(!id)id=leader_id;if(!group)group=id;
    Lease target,owner;pid_t host;int result=target_locked(id,&host,&target);
    if(!result&&id!=leader_id&&leases[id].child!=host){errno=ESRCH;result=-1;}
    int fd=!result?leases[id].descriptor-1:-1,locked=0;
    if(!result){result=lock_process_state(fd);locked=!result;}
    if(!result)result=record_locked(id,&target,!!leases[id].child);
    if(!result){
        if(group==id)owner=target;
        else if(record_locked(group,&owner,1))result=-1;
        else if(owner.kind!=1||owner.process_id!=group||!owner.group_leader){errno=EPERM;result=-1;}
    }
    if(!result)result=setpgid(host,owner.host_pid);
    if(!result){
        target.group=group;if(group==id)target.group_leader=1;
        result=write_record(fd,&target);
    }
    if(locked){int error=errno;(void)set_lock(fd,F_UNLCK,2,1);errno=error;}
    unlock_ids();return result;
}
int klj_process_group_signal(pid_t group,int signal){
    if(group<0){errno=EINVAL;return -1;}
    lock_ids();
    if(!group)group=group_locked(leader_id,0);
    Lease owner;int result=-1,authorized=0;
    if(group>0&&!record_locked(group,&owner,1)&&owner.kind==1&&owner.group_leader){
        // An inherited launcher/terminal group is outside this namespace.
        // Only this process or its kernel-held child leases can authorize a
        // physical process group; an untrusted numeric ID alone never can.
        authorized=getpgrp()==owner.host_pid;
        for(int id=KLJ_PROCESS_ID_MIN;id<=KLJ_PROCESS_ID_MAX&&!authorized;id++)
            if(leases[id].child&&getpgid(leases[id].child)==owner.host_pid)authorized=1;
        if(authorized)result=killpg(owner.host_pid,signal);
    }
    if(!authorized)errno=EPERM;
    unlock_ids();return result;
}
int klj_process_signal(pid_t id,int signal){
    if(id==0)return klj_process_group_signal(0,signal);
    if(id<-1&&id!=INT32_MIN)return klj_process_group_signal(-id,signal);
    lock_ids();pid_t physical=-1;
    if(id==leader_id)physical=getpid();
    else if(id>=KLJ_PROCESS_ID_MIN&&id<=KLJ_PROCESS_ID_MAX)physical=leases[id].child;
    int result;
    if(physical<=0){errno=EPERM;result=-1;}else result=kill(physical,signal);
    unlock_ids();return result;
}
/* All wait syscalls are nonblocking while holding the lease mutex. Blocking
 * waits sleep outside it and retry only these authenticated children; Darwin's
 * wait(-1) would also reap unrelated JVM/ProcessBuilder children. */
static int matches_child_locked(int id,pid_t selector){
    if(!leases[id].child)return 0;
    if(selector>0)return id==selector;
    if(selector==-1)return 1;
    pid_t group=selector==0?group_locked(leader_id,0):-selector;
    if(group<0)return 0;
    Lease record;if(record_locked(id,&record,1))return 0;
    return record.group==group;
}
static int wait_event(void){
    int fd=kqueue();if(fd<0)return -1;
    if(fcntl(fd,F_SETFD,FD_CLOEXEC)){int error=errno;close(fd);errno=error;return -1;}
    struct kevent event;EV_SET(&event,SIGCHLD,EVFILT_SIGNAL,EV_ADD|EV_CLEAR,0,0,NULL);
    if(kevent(fd,&event,1,NULL,0,NULL)<0){int error=errno;close(fd);errno=error;return -1;}
    return fd;
}
static pid_t wait_once(pid_t selector,int *status,siginfo_t *info,int options,struct rusage *usage){
    int found=0;lock_ids();
    for(int id=KLJ_PROCESS_ID_MIN;id<=KLJ_PROCESS_ID_MAX;id++)if(matches_child_locked(id,selector)){
        found++;pid_t host=leases[id].child;int native_status=0,complete=0,reaped=0,error=0;
        siginfo_t state={0};
        if(info){
            error=waitid(P_PID,(id_t)host,&state,options|WNOHANG);
            complete=!error&&state.si_pid;
            reaped=complete&&!(options&WNOWAIT)&&
                (state.si_code==CLD_EXITED||state.si_code==CLD_KILLED||state.si_code==CLD_DUMPED);
        }else{
            pid_t result=wait4(host,&native_status,options|WNOHANG,usage);
            error=result<0?-1:0;complete=result>0;
            reaped=complete&&(WIFEXITED(native_status)||WIFSIGNALED(native_status));
        }
        if(complete){
            if(WIFSIGNALED(native_status))native_status=(native_status&~127)|signal_from_record(leases[id].descriptor-1,WTERMSIG(native_status));
            if(state.si_code==CLD_KILLED||state.si_code==CLD_DUMPED)state.si_status=signal_from_record(leases[id].descriptor-1,state.si_status);
            if(status)*status=native_status;
            if(info){state.si_pid=id;*info=state;}
            if(reaped)release_child_locked(id);
            unlock_ids();return id;
        }
        if(error&&errno!=ECHILD){int saved=errno;unlock_ids();errno=saved;return -1;}
        if(error){release_child_locked(id);found--;}
    }
    unlock_ids();
    if(!found){errno=ECHILD;return -1;}return 0;
}
static pid_t wait_common(pid_t selector,int *status,siginfo_t *info,int options,struct rusage *usage){
    if(selector==INT32_MIN){errno=ESRCH;return -1;}
    // Register before inspecting children, closing the exit-notification race.
    // This observes SIGCHLD without replacing the process's signal handler.
    int event=-1;if(!(options&WNOHANG)){event=wait_event();if(event<0)return -1;}
    pid_t result;
    for(;;){
        result=wait_once(selector,status,info,options,usage);
        if(result||event<0)break;
        struct kevent change;
        if(kevent(event,NULL,0,&change,1,NULL)<0){result=-1;break;}
    }
    int error=errno;if(event>=0)close(event);errno=error;
    if(!result&&info)memset(info,0,sizeof(*info));
    return result;
}
pid_t klj_process_wait_native(pid_t selector,int *status,int options,struct rusage *usage){
    return wait_common(selector,status,NULL,options,usage);
}
int klj_process_waitid_native(idtype_t type,id_t requested,siginfo_t *info,int options){
    if(!info){errno=EFAULT;return -1;}
    if(type!=P_ALL&&type!=P_PID&&type!=P_PGID){errno=EINVAL;return -1;}
    if(requested>INT32_MAX||(type==P_PID&&!requested)){errno=EINVAL;return -1;}
    pid_t selector=type==P_ALL?-1:type==P_PID?(pid_t)requested:-(pid_t)requested;
    return wait_common(selector,NULL,info,options,NULL)<0?-1:0;
}
int klj_process_child_id(pid_t host){
    // SIGCHLD conversion may run in a signal handler. These atomic slots are
    // the same kernel lease owners; no locks, allocations or filesystem I/O.
    // A process cannot be its own child. Procfs self lookups take this path
    // before the live-owner lookup; avoid scanning every compact ID for them.
    if(host<=0||host==getpid())return 0;
    for(int id=KLJ_PROCESS_ID_MIN;id<=KLJ_PROCESS_ID_MAX;id++)
        if(atomic_load_explicit(&child_hosts[id],memory_order_acquire)==host)return id;
    return 0;
}
