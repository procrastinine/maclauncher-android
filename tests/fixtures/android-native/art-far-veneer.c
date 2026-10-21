#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static const size_t page=16384, span=512UL<<20;
static volatile sig_atomic_t traps;
static void trap(int signal,siginfo_t *info,void *context) {
    assert(signal==SIGTRAP&&info);
    if(info->si_code>0)((ucontext_t *)context)->uc_mcontext.pc+=4;
    traps++;
}
static int source(const char *name,const uint32_t *words,size_t count) {
    int fd=open(name,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);assert(fd>=0);
    assert(!ftruncate(fd,page));assert(pwrite(fd,words,count*4,0)==(ssize_t)count*4);close(fd);
    fd=open(name,O_RDONLY|O_CLOEXEC);assert(fd>=0);return fd;
}
static void *load(void *site,int fd) {
    return mmap(site,page,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_FIXED,fd,0);
}
static uint64_t call(void *site,uint64_t input) {
    return ((uint64_t(*)(uint64_t))site)(input);
}
static void readable_executable(void *site) {
    FILE *file=fopen("/proc/self/maps","r");assert(file);char line[1024],permissions[8];unsigned long lo,hi;int found=0;
    while(fgets(line,sizeof line,file))if(sscanf(line,"%lx-%lx %7s",&lo,&hi,permissions)==3&&lo<=(uintptr_t)site&&(uintptr_t)site<hi){assert(!strncmp(permissions,"r-x",3));found=1;break;}
    fclose(file);assert(found);
}
int main(int argc,char **argv) {
    setvbuf(stdout,0,_IONBF,0);
    assert(argc==2&&chdir(argv[1])==0);
    struct sigaction action={.sa_sigaction=trap,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);assert(!sigaction(SIGTRAP,&action,0));
    void *base=mmap(0,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(base!=MAP_FAILED);
    void *site=(char *)base+(256UL<<20);
    /* Every address within a direct branch of this original instruction is
     * occupied. Only original instruction hardware veneers may execute it. */
    const uint32_t first[]={0xaa0003f2,0xd1038240,0xd65f03c0}; /* mov x18,x0; sub x0,x18,#224; ret */
    const uint32_t second[]={0xaa0003f2,0xd1038640,0xd65f03c0}; /* sub #225 */
    const int a=source("first.code",first,3),b=source("second.code",second,3);
    assert(load(site,a)==site);const uint32_t old=((uint32_t *)site)[0];assert((old&0xffe0001f)==0xd4200000);
    printf("EXECUTE base=%p site=%p word=%08x\n",base,site,old);
    assert(call(site,1000)==776);readable_executable(site);
    puts("PASS original far instruction hardware semantics");
    assert(load(site,b)==site);const uint32_t current=((uint32_t *)site)[0];assert(current!=old);assert(call(site,1000)==775);
    puts("PASS replacement has distinct fetched instruction identity");
    const uint32_t collision[]={old,0xd65f03c0};int fd=source("collision.code",collision,2);
    errno=0;assert(load(site,fd)==MAP_FAILED&&errno==ENOEXEC);close(fd);
    assert(((uint32_t *)site)[0]==current&&call(site,1000)==775);readable_executable(site);
    puts("PASS original BRK collision refusal preserves old bytes and RX permission");
    fd=open("collision.code",O_RDONLY|O_CLOEXEC);assert(fd>=0);
    void *movable=mmap(0,page,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0);assert(movable!=MAP_FAILED);close(fd);
    assert(*(uint32_t *)movable==old);errno=0;
    assert(mremap(movable,page,page,MREMAP_MAYMOVE|MREMAP_FIXED,site)==MAP_FAILED&&errno==ENOEXEC);
    assert(*(uint32_t *)movable==old&&((uint32_t *)site)[0]==current&&call(site,1000)==775);readable_executable(site);assert(!munmap(movable,page));
    puts("PASS original BRK move collision preserves source and destination");
    const uint32_t refused[]={0xd71f0a40,0xd65f03c0};fd=source("refused.code",refused,2);
    errno=0;assert(load(site,fd)==MAP_FAILED&&errno==ENOEXEC);close(fd);
    assert(((uint32_t *)site)[0]==current&&call(site,1000)==775);readable_executable(site);
    puts("PASS translation refusal preserves old bytes and RX permission");
    const uint32_t original_trap[]={0xd4200000|(0x1234<<5),0xd65f03c0};fd=source("trap.code",original_trap,2);
    assert(load(site,fd)==site);assert(((uint32_t *)site)[0]==original_trap[0]);assert(call(site,17)==17&&traps==1);close(fd);
    assert(!raise(SIGTRAP)&&traps==2);
    puts("PASS original and software SIGTRAP delivery");
    const uint32_t stack[]={0x910003f2,0xd100425f,0x910043ff,0xd65f03c0}; /* mov x18,sp; sub sp,x18,#16; add sp,sp,#16; ret */
    fd=source("stack.code",stack,4);assert(load(site,fd)==site);assert(call(site,37)==37);close(fd);
    puts("PASS far dynamic SP completion preserves stack and registers");
    const uint32_t address[]={0x90000012,0xaa1203e0,0xd65f03c0}; /* adrp x18,.; mov x0,x18; ret */
    fd=source("address.code",address,3);assert(load(site,fd)==site);assert(call(site,0)==((uintptr_t)site&~4095UL));close(fd);
    puts("PASS far PC relative instruction preserves original address");
    assert(load(site,a)==site&&call(site,1000)==776);
    void *other=mmap(0,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(other!=MAP_FAILED);
    void *destination=(char *)other+(256UL<<20);
    assert(mremap(site,page,page,MREMAP_MAYMOVE|MREMAP_FIXED,destination)==destination);
    assert(call(destination,1000)==776);readable_executable(destination);
    puts("PASS moved far instruction retains hardware semantics");
    assert(mremap(destination,page,page,MREMAP_MAYMOVE|MREMAP_FIXED|MREMAP_DONTUNMAP,site)==site);
    assert(call(site,1000)==776&&call(destination,1000)==776);destination=site;
    puts("PASS DONTUNMAP retains executable source and destination owners");
    pid_t child=fork();assert(child>=0);if(!child){assert(call(destination,1000)==776);_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    puts("PASS fork restores immutable far instruction owners");
    assert(!munmap(destination,page));errno=0;assert(mprotect(destination,page,PROT_READ|PROT_WRITE)==-1&&errno==ENOMEM);
    close(a);close(b);assert(!munmap(base,span));assert(!munmap(other,span));return 0;
}
