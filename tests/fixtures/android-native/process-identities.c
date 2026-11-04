#include "process-ids.h"
#include "thread-identity.h"
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <mach/notify.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern int _tlv_atexit(void (*)(void *),void *);
static const char *directory;
static pthread_key_t late_key;
static _Atomic int callbacks,ready,leave;
typedef struct {int reserved,actual;} Worker;
static void inspect(int id,int expected_kind,int process,pid_t physical){
    pid_t host=0;int kind=0,parent=0;
    assert(!klj_process_id_owner(directory,id,&host,&kind,&parent));
    assert(host==physical&&kind==expected_kind&&parent==process);
}
static void late(void *value){
    Worker *worker=value;
    assert(klj_thread_id()==worker->actual);
    inspect(worker->actual,2,klj_process_id(),getpid());
    atomic_fetch_add(&callbacks,1);
}
static void *run(void *value){
    Worker *worker=value;
    worker->actual=worker->reserved?klj_thread_id_adopt(worker->reserved):klj_thread_id();
    assert(worker->actual>=2&&worker->actual<=65535);
    assert(!worker->reserved||worker->actual==worker->reserved);
    assert(klj_thread_id()==worker->actual);
    assert(!pthread_setspecific(late_key,worker));
    assert(!_tlv_atexit(late,worker));
    atomic_fetch_add(&ready,1);
    while(!atomic_load(&leave))usleep(1000);
    return NULL;
}
/* A trace status retains an ID, not its physical thread or executor state.
 * Condition variables control guest exit; a Mach dead-name notification
 * establishes physical retirement independently of pthread_join. */
typedef struct {
    int reserved,actual,ready,leave;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    _Atomic int callbacks,retired;
} HeldWorker;
static pthread_key_t held_key;
static void held_retire(void *value){
    HeldWorker *worker=value;
    assert(atomic_fetch_add(&worker->retired,1)==0);
}
static void held_late(void *value){
    HeldWorker *worker=value;
    assert(klj_thread_id()==worker->actual);
    assert(klj_thread_memory_context()==worker);
    assert(!atomic_load(&worker->retired));
    inspect(worker->actual,2,klj_process_id(),getpid());
    atomic_fetch_add(&worker->callbacks,1);
}
static void *held_run(void *value){
    HeldWorker *worker=value;
    worker->actual=klj_thread_id_adopt(worker->reserved);
    assert(worker->actual==worker->reserved);
    assert(!klj_thread_memory_adopt(worker,held_retire));
    assert(!pthread_setspecific(held_key,worker));
    assert(!_tlv_atexit(held_late,worker));
    assert(!pthread_mutex_lock(&worker->mutex));
    worker->ready=1;
    assert(!pthread_cond_signal(&worker->changed));
    while(!worker->leave)assert(!pthread_cond_wait(&worker->changed,&worker->mutex));
    assert(!pthread_mutex_unlock(&worker->mutex));
    return NULL;
}
static pthread_t held_start(HeldWorker *worker){
    worker->reserved=klj_thread_id_reserve();assert(worker->reserved>0);
    pthread_t thread;assert(!pthread_create(&thread,NULL,held_run,worker));
    assert(!pthread_mutex_lock(&worker->mutex));
    while(!worker->ready)assert(!pthread_cond_wait(&worker->changed,&worker->mutex));
    assert(!pthread_mutex_unlock(&worker->mutex));
    assert(worker->actual==worker->reserved);
    return thread;
}
static void held_finish(HeldWorker *worker,pthread_t thread){
    /* Keep a separate test-owned right and subscribe before releasing the
     * worker. pthread_join can finish before its Mach thread actually dies. */
    mach_port_t task=mach_task_self(),notice=MACH_PORT_NULL,previous=MACH_PORT_NULL;
    thread_t physical=klj_thread_acquire(worker->actual);assert(physical!=MACH_PORT_NULL);
    assert(mach_port_allocate(task,MACH_PORT_RIGHT_RECEIVE,&notice)==KERN_SUCCESS);
    assert(mach_port_request_notification(task,physical,MACH_NOTIFY_DEAD_NAME,0,
        notice,MACH_MSG_TYPE_MAKE_SEND_ONCE,&previous)==KERN_SUCCESS);
    assert(previous==MACH_PORT_NULL);
    assert(!pthread_mutex_lock(&worker->mutex));
    worker->leave=1;
    assert(!pthread_cond_signal(&worker->changed));
    assert(!pthread_mutex_unlock(&worker->mutex));
    assert(!pthread_join(thread,NULL));
    assert(atomic_load(&worker->callbacks)==2);
    struct {mach_dead_name_notification_t message;mach_msg_max_trailer_t trailer;} death={0};
    assert(mach_msg(&death.message.not_header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,
        sizeof death,notice,5000,MACH_PORT_NULL)==MACH_MSG_SUCCESS);
    assert(death.message.not_header.msgh_id==MACH_NOTIFY_DEAD_NAME);
    assert(death.message.not_port==physical);
    /* Dead-name delivery adds a reference to not_port. Release that and the
     * acquired test right, leaving the native owner's own right for its reap. */
    assert(mach_port_deallocate(task,death.message.not_port)==KERN_SUCCESS);
    assert(mach_port_deallocate(task,physical)==KERN_SUCCESS);
    assert(mach_port_mod_refs(task,notice,MACH_PORT_RIGHT_RECEIVE,-1)==KERN_SUCCESS);
    /* This collects the dead Mach target. Retention must not retain its
     * executor context or expose a physically dead scheduling target. */
    errno=0;assert(klj_thread_acquire(worker->actual)==MACH_PORT_NULL&&errno==ESRCH);
    assert(!klj_thread_priority_target(worker->actual));
    assert(atomic_load(&worker->retired)==1);
}
static void held_destroy(HeldWorker *worker){
    assert(!pthread_cond_destroy(&worker->changed));
    assert(!pthread_mutex_destroy(&worker->mutex));
}
static void assert_not_reused(int retained){
    int other=klj_thread_id_allocate();assert(other>0&&other!=retained);
    klj_thread_id_release(other);
}
static void held_query(int id,int pid){
    pid_t host;int kind,process;
    assert(!klj_process_id_query(id,&host,&kind,&process));
    assert(host==getpid()&&kind==2&&process==pid);
}
static void trace_retention(int pid){
    assert(!pthread_key_create(&held_key,held_late));
    HeldWorker traced={.mutex=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};
    pthread_t thread=held_start(&traced);int id=traced.actual;
    assert(klj_thread_id_hold(id)==id);
    assert(klj_thread_id_hold(id)==id);
    inspect(id,2,pid,getpid());
    held_finish(&traced,thread);
    inspect(id,2,pid,getpid());
    held_query(id,pid);
    assert_not_reused(id);
    assert(klj_thread_id_unhold(id)==id);
    inspect(id,2,pid,getpid());
    held_query(id,pid);
    assert_not_reused(id);
    /* A fork must not inherit authority to consume its parent's trace hold.
     * The child can still observe that lease through the shared owner. */
    pid_t child=fork();assert(child>=0);
    if(!child){
        assert(klj_process_id()!=pid);
        errno=0;assert(klj_thread_id_unhold(id)<0&&errno==ESRCH);
        inspect(id,2,pid,getppid());
        assert_not_reused(id);
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    inspect(id,2,pid,getpid());
    assert_not_reused(id);
    assert(klj_thread_id_unhold(id)==id);
    pid_t host;int kind,process;
    errno=0;assert(klj_process_id_query(id,&host,&kind,&process)<0&&errno==ESRCH);
    errno=0;assert(klj_thread_id_unhold(id)<0&&errno==ESRCH);
    int reused=klj_thread_id_allocate();assert(reused==id);klj_thread_id_release(reused);
    assert(atomic_load(&traced.retired)==1);
    held_destroy(&traced);

    /* Consuming a wait status before physical death cannot retire a live
     * thread. Its later POSIX and TLV callbacks still observe the same ID. */
    HeldWorker early={.mutex=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};
    thread=held_start(&early);id=early.actual;
    assert(klj_thread_id_hold(id)==id);
    assert(klj_thread_id_unhold(id)==id);
    inspect(id,2,pid,getpid());
    thread_t live=klj_thread_acquire(id);assert(live!=MACH_PORT_NULL);
    assert(mach_port_deallocate(mach_task_self(),live)==KERN_SUCCESS);
    assert(!atomic_load(&early.retired));
    assert_not_reused(id);
    held_finish(&early,thread);
    errno=0;assert(klj_process_id_query(id,&host,&kind,&process)<0&&errno==ESRCH);
    reused=klj_thread_id_allocate();assert(reused==id);klj_thread_id_release(reused);
    held_destroy(&early);
    assert(!pthread_key_delete(held_key));
    puts("PASS trace holds retain only IDs through physical retirement, counted wait release, exact TID reuse, early wait TLS identity and fork isolation");
}
int main(int argc,char **argv){
    assert(argc==2);directory=argv[1];
    int pid=klj_process_ids_configure(directory);assert(pid>=2&&pid<=65535);
    assert(klj_thread_id()==pid);
    assert(klj_process_id_from_host(getpid())==pid);
    errno=E2BIG;assert(!klj_process_child_id(getpid())&&errno==E2BIG);
    assert(!klj_process_child_id(0)&&!klj_process_child_id(-1));
    inspect(pid,1,pid,getpid());
    assert(!pthread_key_create(&late_key,late));
    Worker workers[32]={0};pthread_t threads[32];
    for(int i=0;i<32;i++){
        if(i%2)workers[i].reserved=klj_thread_id_reserve();
        assert(!pthread_create(&threads[i],NULL,run,&workers[i]));
    }
    while(atomic_load(&ready)!=32)usleep(1000);
    for(int i=0;i<32;i++){
        inspect(workers[i].actual,2,pid,getpid());
        assert(workers[i].actual!=pid);
        for(int j=0;j<i;j++)assert(workers[i].actual!=workers[j].actual);
    }
    atomic_store(&leave,1);
    for(int i=0;i<32;i++)assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(&callbacks)==64);
    /* Target lookup collects only physically dead Mach threads. All actual
     * TLS and POSIX destructor callbacks above retained their original ID. */
    for(int i=0;i<32;i++){
        assert(klj_thread_acquire(workers[i].actual)==MACH_PORT_NULL&&errno==ESRCH);
        pid_t host;int kind,process;
        assert(klj_process_id_query(workers[i].actual,&host,&kind,&process)<0&&errno==ESRCH);
    }
    trace_retention(pid);
    int messages[2],ack[2];assert(!pipe(messages)&&!pipe(ack));
    pid_t child=fork();assert(child>=0);
    if(!child){
        close(messages[0]);close(ack[1]);
        int child_pid=klj_process_id();assert(child_pid>=2&&child_pid<=65535&&child_pid!=pid);
        assert(klj_thread_id()==child_pid);
        assert(!klj_process_child_id(getpid()));
        assert(write(messages[1],&child_pid,sizeof child_pid)==sizeof child_pid);
        char byte;assert(read(ack[0],&byte,1)==1);_exit(0);
    }
    close(messages[1]);close(ack[0]);int child_pid;
    assert(read(messages[0],&child_pid,sizeof child_pid)==sizeof child_pid);
    for(int i=0;i<64;i++)assert(klj_process_id_from_host(child)==child_pid);
    assert(klj_process_child_retain(child)==child_pid);
    assert(klj_process_child_host(child_pid)==child);
    assert(klj_process_child_id(child)==child_pid);
    errno=ESPIPE;assert(!klj_process_child_id(getpid())&&errno==ESPIPE);
    assert(write(ack[1],"x",1)==1);
    siginfo_t state={0};assert(!waitid(P_PID,(id_t)child,&state,WEXITED|WNOWAIT));
    assert(klj_process_child_id(child)==child_pid);
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status));
    assert(klj_process_child_reaped(child)==child_pid);
    assert(!klj_process_child_host(child_pid));
    assert(!klj_process_child_id(child));
    close(messages[0]);close(ack[1]);pthread_key_delete(late_key);
    // A burst determines the high-water mark. Sequential short-lived threads
    // reuse it instead of creating one disk block for every historical ID.
    DIR *entries=opendir(directory);assert(entries);size_t before=0,after=0;struct dirent *entry;
    while((entry=readdir(entries)))if(entry->d_name[0]>='0'&&entry->d_name[0]<='9')before++;
    closedir(entries);
    for(int i=0;i<4096;i++){int id=klj_thread_id_allocate();assert(id>0);klj_thread_id_release(id);}
    entries=opendir(directory);assert(entries);
    while((entry=readdir(entries)))if(entry->d_name[0]>='0'&&entry->d_name[0]<='9')after++;
    closedir(entries);assert(after==before&&after<=35);
    // Kernel locks release on abrupt process death, without user-space
    // cleanup. A new physical owner can reclaim the same stable inode.
    int released=klj_thread_id_allocate();klj_thread_id_release(released);
    child=fork();assert(child>=0);
    if(!child){assert(klj_process_id()==released);_exit(0);}
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status));
    assert(klj_thread_id_allocate()==released);klj_thread_id_release(released);
    puts("PASS shared compact process/thread leases, reserved adoption, late TLS identity, Mach-death retirement, repeated reverse lookup and retained child wait");
}
