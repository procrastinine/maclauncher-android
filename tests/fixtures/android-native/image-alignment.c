#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc,char **argv){
 assert(argc==4);size_t page=getpagesize();assert(page==16384);
 void *core=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);if(!core){fprintf(stderr,"%s\n",dlerror());return 2;}
 int (*configure)(const char *)=dlsym(core,"klj_process_ids_configure");assert(configure&&configure(argv[3])>0);
 void *(*load)(const char *)=dlsym(core,"kl_load_recursive"),*(*symbol)(void *,const char *)=dlsym(core,"kl_sym"),*(*base_of)(void *)=dlsym(core,"kl_base");
 size_t (*span_of)(void *)=dlsym(core,"kl_span");void (*unload)(void *)=dlsym(core,"kl_unload");
 int (*claim)(void *,size_t,int)=dlsym(core,"klj_guest_mapping_claim"),(*owned)(void *,size_t)=dlsym(core,"klj_guest_mapping_owned");
 void *(*lookup)(const char *)=dlsym(core,"kl_shim_lookup");
 assert(load&&symbol&&base_of&&span_of&&unload&&claim&&owned&&lookup);
 void *image=load(argv[2]);assert(image);uintptr_t base=(uintptr_t)base_of(image);size_t span=span_of(image);
 assert(base%page==12288&&span>4096&&span<page);
 uintptr_t first=base&~(uintptr_t)(page-1),last=(base+span+page-1)&~(uintptr_t)(page-1);
 assert(last-first==2*page&&owned((void *)first,last-first));
 int (*value)(void)=symbol(image,"shifted_value");const uint32_t *data=symbol(image,"shifted_data");
 assert(value&&value()==42&&data&&*data==0x11223344);
 unload(image);assert(!owned((void *)first,last-first));
 puts("PASS shifted 4KiB ELF publishes, executes and retires its covering 16KiB ownership pages");
 void *allocation=mmap(NULL,4*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(allocation!=MAP_FAILED);
 unsigned char *bytes=allocation;bytes[0]=19;bytes[3*page]=23;
 assert(!claim(bytes+page+12288,8192,1));assert(owned(bytes+page,2*page));assert(!owned(bytes,page)&&!owned(bytes+3*page,page));
 int (*protect)(void *,size_t,int)=lookup("mprotect");assert(protect);errno=0;
 assert(protect(bytes+page+12288,page,PROT_READ|PROT_WRITE|PROT_EXEC)==-1&&errno==EINVAL);
 assert(!claim(bytes+page+12288,8192,0)&&!owned(bytes+page,2*page));
 errno=0;assert(claim((void *)(UINTPTR_MAX-4095),8192,1)==-1&&errno==EINVAL);
 errno=0;assert(claim((void *)(UINTPTR_MAX-8191),4096,1)==-1&&errno==EINVAL);
 assert(bytes[0]==19&&bytes[3*page]==23&&!owned(bytes,4*page));assert(!munmap(allocation,4*page));
 puts("PASS image claims preserve adjacent ownership, checked overflow and strict syscall alignment");
 return 0;
}
