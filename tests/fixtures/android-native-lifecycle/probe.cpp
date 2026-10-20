#include <assert.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
extern "C" int klj_guest_cxa_atexit(void (*)(void*),void*,void*);
extern "C" void klj_guest_cxa_finalize(void*);
extern "C" void klj_guest_exit(int);
extern "C" int klj_guest_cxa_thread_atexit_impl(void (*)(void*),void*,void*);
extern "C" int klj_guest_register_atfork(void (*)(void),void (*)(void),void (*)(void),void*);
static int firstDso,secondDso,forkDso;
static std::vector<int> events;
static std::atomic<unsigned> count;
static void record(void* value) { events.push_back(int(reinterpret_cast<intptr_t>(value))); }
static void append(void*) {
    record(reinterpret_cast<void*>(2));
    assert(!klj_guest_cxa_atexit(record,reinterpret_cast<void*>(3),&firstDso));
    klj_guest_cxa_finalize(&firstDso);
}
static void increment(void*) { count.fetch_add(1); }
static int forks,childForks;
static void prepare() { ++forks; }
static void parent() { ++forks; }
static void child() { ++childForks; }
static void threadExitProof(void*) { events.push_back(6); }
static void exitProof(void*) {
    assert(events.back()==6);
    puts("PASS native exit thread/global finalization and status");
}
static void hostExitMustNotRun() { abort(); }
static void hostMustNotFinalize(void*) { abort(); }
int main() {
    assert(!klj_guest_cxa_atexit(record,reinterpret_cast<void*>(1),&firstDso));
    assert(!klj_guest_cxa_atexit(record,reinterpret_cast<void*>(4),&secondDso));
    assert(!klj_guest_cxa_atexit(append,nullptr,&firstDso));
    klj_guest_cxa_finalize(&firstDso);
    assert((events==std::vector<int>{2,3,1}));
    klj_guest_cxa_finalize(&firstDso);
    assert(events.size()==3);
    klj_guest_cxa_finalize(&secondDso);
    assert(events.back()==4);
    // Expansion, concurrent extraction, new registrations and compaction all
    // exercise the original Bionic algorithm rather than a mirrored model.
    for(int i=0;i<10000;i++)assert(!klj_guest_cxa_atexit(increment,nullptr,i%2?&firstDso:&secondDso));
    std::thread left([]{klj_guest_cxa_finalize(&firstDso);});
    std::thread right([]{klj_guest_cxa_finalize(&secondDso);});left.join();right.join();
    assert(count==10000);
    assert(!klj_guest_register_atfork(prepare,parent,child,&forkDso));
    pid_t pid=fork();assert(pid>=0);
    if(!pid)_exit(childForks==1?0:1);
    int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    assert(forks==2);
    klj_guest_cxa_finalize(&forkDso);
    pid=fork();assert(pid>=0);if(!pid)_exit(childForks==0?0:1);
    assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    assert(forks==2);
    klj_guest_cxa_finalize(nullptr); // Must not destroy this host vector/JVM.
    events.push_back(5);assert(events.back()==5);
    pid=fork();assert(pid>=0);
    if(!pid){
        assert(!atexit(hostExitMustNotRun));
        assert(!klj_guest_cxa_thread_atexit_impl(threadExitProof,nullptr,&firstDso));
        assert(!klj_guest_cxa_atexit(exitProof,nullptr,&firstDso));
        klj_guest_exit(37);_exit(99);
    }
    assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==37);
    // Host/JVM retirement does not simulate a guest exit after an engine has
    // already released its own globals. This callback must remain uncalled.
    assert(!klj_guest_cxa_atexit(hostMustNotFinalize,nullptr,&firstDso));
    puts("PASS DSO isolation, recursion, 10000 concurrent finalizers and atfork removal");
}
