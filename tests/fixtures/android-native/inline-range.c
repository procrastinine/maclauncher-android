#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "inline-syscall.h"
static void read_execute_only(void *pointer){
 mach_vm_address_t address=(uintptr_t)pointer;mach_vm_size_t size=0;vm_region_basic_info_data_64_t info;
 mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
 assert(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
 if(object)mach_port_deallocate(mach_task_self(),object);
 assert(info.protection==(VM_PROT_READ|VM_PROT_EXECUTE));
}
int main(int argc,char **argv){
 assert(argc==3);void *core=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);if(!core){puts(dlerror());return 2;}
 int (*configure)(const char *)=dlsym(core,"klj_process_ids_configure");assert(configure&&configure(getenv("KL_ANDROID_ID_DIRECTORY"))>0);
 void (*enter)(void)=dlsym(core,"kl_thread_init");void *(*pointer)(void)=dlsym(core,"klj_guest_thread_pointer");assert(enter&&pointer);enter();
 void *(*load)(const char *)=dlsym(core,"kl_load"),*(*symbol)(void *,const char *)=dlsym(core,"kl_sym");
 const char *(*error)(void)=dlsym(core,"kl_error");void (*unload)(void *)=dlsym(core,"kl_unload");assert(load&&symbol&&error&&unload);
 void *image=load(argv[2]);if(!image){fprintf(stderr,"%s\n",error());return 3;}
 void *(*first)(void)=symbol(image,"range_first"),*(*last)(void)=symbol(image,"range_last");
 long (*middle)(void)=symbol(image,"range_middle");const uint64_t *data=symbol(image,"range_original_data");
 assert(first&&middle&&last&&data&&*data==UINT64_C(0x1020304050607080));
 assert(first()==pointer()&&last()==pointer());
 uint32_t entry=((const uint32_t *)(void *)middle)[1];assert((entry&0xfc000000u)==0x14000000u);
 int64_t displacement=(int64_t)((int32_t)(entry<<6)>>6)*4;
 void *island=(char *)(void *)middle+4+displacement;
 read_execute_only(island);read_execute_only((void *)middle);
 long (*syscall)(long,long,long,long,long,long,long)=dlsym(core,"klb_syscall");assert(syscall&&middle()==syscall(178,0,0,0,0,0,0));
 assert(*data==UINT64_C(0x1020304050607080));unload(image);
 int (*patch)(void *,size_t,KljInlinePool *,size_t,const klx_code_context *)=dlsym(core,"klj_inline_syscall_patch_pools_context");assert(patch);
 size_t size=0x10410000,page=(size_t)getpagesize();
 uint8_t *map=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);assert(map!=MAP_FAILED);
 uint32_t words[]={0xd2801648,0xd4000001,0xd65f03c0};uint32_t *code=(void *)(map+0x08208000);memcpy(code,words,sizeof words);
 uint64_t boundaries[]={(uintptr_t)code,(uintptr_t)code+sizeof words};
 klx_instruction_range reachable={(uintptr_t)code,(uintptr_t)code+sizeof words};
 klx_code_context context={boundaries,2,(uintptr_t)code,&reachable,1};
 KljInlinePool pools[]={{map,page,0,0},{map+size-page,page,0,0}};
 errno=0;assert(patch(code,sizeof words,pools,2,&context)<0&&errno==ERANGE);
 assert(!memcmp(code,words,sizeof words)&&!pools[0].used&&!pools[1].used);
 assert(!munmap(map,size));
 puts("PASS sparse large ELF executes original TP/SVC through outer and interior pools; unreachable plans leave every original instruction unchanged");
}
