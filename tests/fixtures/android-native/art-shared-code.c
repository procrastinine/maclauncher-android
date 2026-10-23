#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static const size_t page=16384;
// Preserve the original Android x18 ABI while exercising real x18 instructions.
static const uint32_t first[]={0xaa1203e9,0xaa0003f2,0xd1038240,0xaa0903f2,0xd65f03c0};
static const uint32_t second[]={0xaa1203e9,0xaa0003f2,0xd1038640,0xaa0903f2,0xd65f03c0};
static uintptr_t expected_bus;
static void bus(int signal,siginfo_t *info,void *context){
    (void)context;
    if(signal==SIGBUS&&info&&info->si_code>0&&(uintptr_t)info->si_addr>=expected_bus&&(uintptr_t)info->si_addr<expected_bus+page)_exit(0);
    _exit(121);
}
static uint64_t call(void *code){return ((uint64_t(*)(uint64_t))code)(1000);}
int main(int argc,char **argv){
    setvbuf(stdout,0,_IONBF,0);assert(argc==2&&!chdir(argv[1]));alarm(10);
    int fd=open("original-shared.code",O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);assert(fd>=0);assert(!ftruncate(fd,2*page));
    assert(pwrite(fd,first,sizeof first,0)==sizeof first&&pwrite(fd,first,sizeof first,page)==sizeof first);
    void *code=mmap(0,2*page,PROT_READ|PROT_EXEC,MAP_SHARED,fd,0);assert(code!=MAP_FAILED);
    void *data=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(data!=MAP_FAILED);
    void *other=mmap(0,2*page,PROT_READ|PROT_EXEC,MAP_SHARED,fd,0);assert(other!=MAP_FAILED);
    assert(!memcmp(code,first,sizeof first)&&!memcmp(other,first,sizeof first)&&call(code)==776&&call(other)==776);
    puts("PASS original shared code keeps original bytes and executes through both views");
    ((volatile uint32_t*)data)[2]=second[2];assert(call(code)==775&&call(other)==775&&!memcmp(code,second,sizeof second));
    assert(!msync(code,2*page,MS_SYNC));uint32_t exact[3];assert(pread(fd,exact,sizeof exact,0)==sizeof exact&&!memcmp(exact,second,sizeof exact));
    assert(pwrite(fd,first,sizeof first,0)==sizeof first&&call(code)==776&&call(other)==776);
    puts("PASS shared data writes, msync and file pwrite preserve cross-alias visibility");
    assert(!mprotect(code,2*page,PROT_READ|PROT_WRITE|PROT_EXEC));((volatile uint32_t*)code)[2]=second[2];assert(call(code)==775&&call(other)==775);
    puts("PASS original shared executable view accepts its real writable transition");
    pid_t child=fork();assert(child>=0);if(!child){assert(call(code)==775);((volatile uint32_t*)code)[2]=first[2];assert(call(other)==776);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);assert(call(code)==776&&call(other)==776&&((uint32_t*)data)[2]==first[2]);
    puts("PASS fresh fork child writes stay shared with parent and original file");
    int readonly=open("original-shared.code",O_RDONLY|O_CLOEXEC);assert(readonly>=0);
    void *sentinel=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(sentinel!=MAP_FAILED);memset(sentinel,0x5a,page);
    errno=0;assert(mmap(sentinel,page,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED|MAP_FIXED,readonly,0)==MAP_FAILED&&errno==EACCES);assert(((uint8_t*)sentinel)[0]==0x5a&&((uint8_t*)sentinel)[page-1]==0x5a);
    void *ro=mmap(0,page,PROT_READ|PROT_EXEC,MAP_SHARED,readonly,0);assert(ro!=MAP_FAILED);errno=0;assert(mprotect(ro,page,PROT_READ|PROT_WRITE|PROT_EXEC)==-1&&errno==EACCES);assert(call(ro)==776);
    puts("PASS shared readonly requests refuse writes before fixed target replacement");
    void *alias=mremap(code,0,2*page,MREMAP_MAYMOVE);assert(alias!=MAP_FAILED&&call(alias)==776);((volatile uint32_t*)alias)[2]=second[2];assert(call(code)==775&&call(other)==775);
    assert(!madvise(alias,2*page,MADV_DONTNEED)&&call(code)==775&&call(alias)==775);assert(pread(fd,exact,sizeof exact,0)==sizeof exact&&!memcmp(exact,second,sizeof exact));
    puts("PASS shared remap alias and discard retain actual backing and contents");
    assert(!ftruncate(fd,page));child=fork();assert(child>=0);if(!child){expected_bus=(uintptr_t)code+page;struct sigaction action={.sa_sigaction=bus,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);assert(!sigaction(SIGBUS,&action,0));(void)call((char*)code+page);_exit(122);}
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    assert(pread(fd,exact,sizeof exact,0)==sizeof exact&&!memcmp(exact,second,sizeof exact));
    puts("PASS original truncation delivers SIGBUS without restoring translated file words");
    assert(!munmap(alias,2*page)&&!munmap(code,2*page)&&!munmap(other,2*page)&&!munmap(data,2*page)&&!munmap(ro,page)&&!munmap(sentinel,page));close(readonly);close(fd);unlink("original-shared.code");alarm(0);return 0;
}
