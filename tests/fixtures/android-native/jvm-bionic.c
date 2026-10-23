#define _GNU_SOURCE
#include "jvm-bionic.h"
#include "jvm-bionic-jni.h"
#include "bionic-provider.h"
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int64_t fixture_host(uint32_t function,void *args,size_t length){
 register uint64_t x0 __asm__("x0")=0x4d4c,x1 __asm__("x1")=function,x2 __asm__("x2")=(uintptr_t)args,x3 __asm__("x3")=length,x8 __asm__("x8")=0x48430000;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","x16","x17","x30","memory","cc");return (int64_t)x0;
}
static uint64_t check(uint64_t raw){
 FixtureCall *call=(void*)(uintptr_t)raw;int *(*errno_fn)(void)=(void*)(uintptr_t)call->errno_function;
 assert(errno_fn()==__errno());++call->checks;
 assert(((pid_t(*)(void))(uintptr_t)call->getpid_function)()==getpid());++call->checks;
 assert(((pthread_t(*)(void))(uintptr_t)call->pthread_self_function)()==pthread_self());++call->checks;
 errno=177;assert(*errno_fn()==177);++call->checks;
 void *memory=((void*(*)(void*,size_t,int,int,int,off_t))(uintptr_t)call->mmap_function)(0,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(memory!=MAP_FAILED);memset(memory,0x5a,16384);assert(!((int(*)(void*,size_t))(uintptr_t)call->munmap_function)(memory,16384));++call->checks;
 assert(fixture_host(2,0,0)==0xabc&&*errno_fn()==177);++call->checks;return 0x12345678;
}
static void elf_symbol(void *symbol,const char *name){
 Dl_info info;assert(dladdr(symbol,&info)&&info.dli_fbase&&info.dli_fname);
 int fd=open(info.dli_fname,O_RDONLY|O_CLOEXEC);assert(fd>=0);Elf64_Ehdr header;assert(pread(fd,&header,sizeof header,0)==sizeof header&&!memcmp(header.e_ident,ELFMAG,SELFMAG));
 Elf64_Shdr *sections=malloc(header.e_shnum*sizeof(*sections));assert(sections&&pread(fd,sections,header.e_shnum*sizeof(*sections),header.e_shoff)==(ssize_t)(header.e_shnum*sizeof(*sections)));
 int found=0;for(unsigned n=0;n<header.e_shnum;n++)if(sections[n].sh_type==SHT_DYNSYM){
 Elf64_Shdr *strings=&sections[sections[n].sh_link];char *names=malloc(strings->sh_size);Elf64_Sym *symbols=malloc(sections[n].sh_size);assert(names&&symbols);
 assert(pread(fd,names,strings->sh_size,strings->sh_offset)==(ssize_t)strings->sh_size&&pread(fd,symbols,sections[n].sh_size,sections[n].sh_offset)==(ssize_t)sections[n].sh_size);
 for(unsigned i=0;i<sections[n].sh_size/sizeof(*symbols);i++)if(!strcmp(names+symbols[i].st_name,name)){assert((uintptr_t)info.dli_fbase+symbols[i].st_value==(uintptr_t)symbol);found=1;break;}free(names);free(symbols);
 }assert(found);free(sections);close(fd);
}
static void *thread(void *unused){(void)unused;FixtureRequest request={(uintptr_t)klj_bionic_link_dispatch,(uintptr_t)check,(uintptr_t)dlsym(RTLD_DEFAULT,"getpid"),(uintptr_t)dlsym(RTLD_DEFAULT,"__errno"),(uintptr_t)pthread_self(),0,0};
 int *before=__errno();errno=131;assert(!fixture_host(1,&request,sizeof request)&&request.checks==8&&__errno()==before&&errno==177);return before;
}
static uint64_t typed_dispatch(uint64_t raw){
 FixtureJniCall *call=(void*)(uintptr_t)raw;const jvalue *v=call->values;
 jlong (*function)(JNIEnv*,jclass,jint,jlong,jfloat,jdouble,jint,jint,jint,jint,jint,jint)=(void*)(uintptr_t)call->function;
 call->result=function(call->env,call->type,v[0].i,v[1].j,v[2].f,v[3].d,v[4].i,v[5].i,v[6].i,v[7].i,v[8].i,v[9].i);return 0;
}
static void *jvm_thread(void *unused){(void)unused;FixtureJvm request={(uintptr_t)klj_bionic_link_dispatch,(uintptr_t)typed_dispatch,"/data/local/tmp/bionic-provider/libbionicjni.so"};assert(fixture_host(3,&request,sizeof request)==0);return 0;}
int main(void){setvbuf(stdout,0,_IONBF,0);void *pid=dlsym(RTLD_DEFAULT,"getpid");assert(pid);elf_symbol(pid,"getpid");puts("PASS original libc ELF symbol value matches its callable mapping");
 void *main_tls=thread(0);puts("PASS native host reentry uses original dlopen dlsym TLS errno syscalls and nested host call");
 pthread_t worker;assert(!pthread_create(&worker,0,thread,0));void *child_tls;assert(!pthread_join(worker,&child_tls)&&child_tls!=main_tls);puts("PASS Bionic-created thread keeps independent real TLS across native reentry");
 pthread_t jvm;assert(!pthread_create(&jvm,0,jvm_thread,0)&&!pthread_join(jvm,0));puts("PASS genuine guest JNI typed entry and host VM callback retain Bionic thread ownership");return 0;}
