#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static uint64_t available(void){struct sysinfo value;assert(!sysinfo(&value));return(uint64_t)value.freeram*value.mem_unit;}
static uint64_t difference(uint64_t a,uint64_t b){return a>b?a-b:b-a;}
int main(int argc,char **argv){
    assert(argc==2);const uint64_t expected=strtoull(argv[1],NULL,10);const long page=sysconf(_SC_PAGESIZE);assert(page>0);
    struct sysinfo info;memset(&info,0xa5,sizeof info);assert(!sysinfo(&info));
    assert((uint64_t)info.totalram*info.mem_unit==expected);
    assert((uint64_t)sysconf(_SC_PHYS_PAGES)*(uint64_t)page==expected);
    const uint64_t avphys=(uint64_t)sysconf(_SC_AVPHYS_PAGES)*(uint64_t)page;
    assert(avphys<=expected&&difference(avphys,available())<8*1024*1024);
    assert(info.freeram<=info.totalram&&info.bufferram==0&&info.totalswap==0);
    FILE *file=fopen("/proc/meminfo","rb");assert(file);char line[256];uint64_t total=UINT64_MAX,free_bytes=UINT64_MAX,avail=UINT64_MAX,cached=UINT64_MAX,active=UINT64_MAX;
    while(fgets(line,sizeof line,file)){unsigned long long value;
        if(sscanf(line,"MemTotal: %llu kB",&value)==1)total=value*1024;
        else if(sscanf(line,"MemFree: %llu kB",&value)==1)free_bytes=value*1024;
        else if(sscanf(line,"MemAvailable: %llu kB",&value)==1)avail=value*1024;
        else if(sscanf(line,"Cached: %llu kB",&value)==1)cached=value;
        else if(sscanf(line,"Active: %llu kB",&value)==1)active=value;
    }
    assert(!fclose(file));assert(total==expected&&free_bytes<=total&&avail<=total&&cached==0&&active==0);
    assert(difference(avail,available())<8*1024*1024);
    const size_t size=32*1024*1024;const uint64_t before=available();
    void *memory=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(memory!=MAP_FAILED);
    memset(memory,0x6a,size);const uint64_t after=available();
    assert(before>=after&&before-after>=24*1024*1024);assert(!munmap(memory,size));
    assert(available()+8*1024*1024>=before);
    printf("PASS original Bionic sysinfo, sysconf physical/available pages and proc total=%llu with live bounded private headroom\n",(unsigned long long)expected);
    return 0;
}
