#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static const size_t page=16384;
static const uint32_t original[]={0xaa1203e9,0xaa0003f2,0xd1038240,0xaa0903f2,0xd65f03c0};
int main(int argc,char**argv){
    setvbuf(stdout,0,_IONBF,0);assert(argc==5);
    const char*name=argv[1];const size_t original_size=strtoull(argv[2],0,0);const unsigned phnum=(unsigned)strtoul(argv[3],0,0);const uint64_t phoff=strtoull(argv[4],0,0);
    int fd=open(name,O_RDONLY|O_CLOEXEC);assert(fd>=0);struct stat status;assert(!fstat(fd,&status)&&(size_t)status.st_size==original_size);
    Elf64_Ehdr header;assert(pread(fd,&header,sizeof header,0)==sizeof header);assert(header.e_phnum==phnum&&header.e_phoff==phoff);
    void*raw=mmap(0,(original_size+page-1)&~(page-1),PROT_READ,MAP_PRIVATE,fd,0);assert(raw!=MAP_FAILED&&!memcmp(raw,&header,sizeof header));
    puts("PASS original cached ELF descriptor headers, size and non-executable data mapping");
    void*library=dlopen(name,RTLD_NOW|RTLD_LOCAL);if(!library)fprintf(stderr,"dlopen: %s\n",dlerror());assert(library);
    uint64_t(*value)(uint64_t)=dlsym(library,"fixture_value");uint32_t(*read_word)(const void*)=dlsym(library,"fixture_native_read");uint64_t(*native_value)(void)=dlsym(library,"fixture_native_value");assert(value&&read_word&&native_value);
    assert(!memcmp((const void*)value,original,sizeof original));assert(value(1000)==776);assert(native_value()==42);
    for(unsigned n=0;n<sizeof original/sizeof original[0];n++)assert(read_word((const uint32_t*)value+n)==original[n]);
    puts("PASS initial rewritten page executes while an ordinary native page reads original opcodes");
    Dl_info info;assert(dladdr((void*)value,&info));uint64_t address=(uintptr_t)value-(uintptr_t)info.dli_fbase,offset=UINT64_MAX;
    for(unsigned n=0;n<phnum;n++){Elf64_Phdr segment;assert(pread(fd,&segment,sizeof segment,phoff+n*header.e_phentsize)==sizeof segment);if(segment.p_type==PT_LOAD&&address>=segment.p_vaddr&&address+sizeof original<=segment.p_vaddr+segment.p_filesz)offset=segment.p_offset+address-segment.p_vaddr;}
    assert(offset!=UINT64_MAX);uint32_t words[sizeof original/sizeof original[0]];assert(pread(fd,words,sizeof words,(off_t)offset)==sizeof words&&!memcmp(words,original,sizeof words));assert(!memcmp((const char*)raw+offset,original,sizeof original));
    assert(lseek(fd,(off_t)offset,SEEK_SET)==(off_t)offset&&read(fd,words,sizeof words)==sizeof words&&!memcmp(words,original,sizeof words));
    puts("PASS original file read, pread and mapped code bytes agree before writable enrollment");
    pid_t child=fork();assert(child>=0);if(!child){assert(!memcmp((const void*)value,original,sizeof original)&&value(1000)==776&&read_word((const void*)value)==original[0]);_exit(0);}int result;assert(waitpid(child,&result,0)==child&&WIFEXITED(result)&&WEXITSTATUS(result)==0);
    puts("PASS initial instruction and original-file leases survive fork");
    void*code=(void*)((uintptr_t)value&~(page-1));assert(!mprotect(code,page,PROT_READ|PROT_WRITE|PROT_EXEC));((volatile uint32_t*)value)[2]=0xd1038640;assert(value(1000)==775);
    assert(!madvise(code,page,MADV_DONTNEED)&&value(1000)==776&&!memcmp((const void*)value,original,sizeof original));
    puts("PASS initial owner transitions to mutable execution and reloads original file bytes");
    assert(!dlclose(library));library=dlopen(name,RTLD_NOW|RTLD_LOCAL);assert(library);value=dlsym(library,"fixture_value");assert(value&&value(1000)==776&&!memcmp((const void*)value,original,sizeof original));assert(!dlclose(library));
    assert(!munmap(raw,(original_size+page-1)&~(page-1))&&!close(fd));puts("PASS initial publication retires and reuses mappings without derived data exposure");return 0;
}
