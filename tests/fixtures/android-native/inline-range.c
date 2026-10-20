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
// The opaque statistics buffer is larger than the original x18 record;
// this probe uses no generated headers or launcher process helpers.
static void read_execute_only(void *pointer){
 mach_vm_address_t address=(uintptr_t)pointer;mach_vm_size_t size=0;vm_region_basic_info_data_64_t info;
 mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
 assert(mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS);
 if(object)mach_port_deallocate(mach_task_self(),object);
 assert(info.protection==(VM_PROT_READ|VM_PROT_EXECUTE));
}
int main(int argc,char **argv){
 assert(argc==4);void *core=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);if(!core){puts(dlerror());return 2;}
 int (*configure)(const char *)=dlsym(core,"klj_process_ids_configure");assert(configure&&configure(getenv("KL_ANDROID_ID_DIRECTORY"))>0);
 void (*enter)(void)=dlsym(core,"kl_thread_init");void *(*pointer)(void)=dlsym(core,"klj_guest_thread_pointer");assert(enter&&pointer);enter();
 void *(*load)(const char *)=dlsym(core,"kl_load"),*(*symbol)(void *,const char *)=dlsym(core,"kl_sym");
 const char *(*error)(void)=dlsym(core,"kl_error");void (*unload)(void *)=dlsym(core,"kl_unload");assert(load&&symbol&&error&&unload);
 void *image=load(argv[2]);if(!image){fprintf(stderr,"%s\n",error());return 3;}
 void *(*first)(void)=symbol(image,"range_first"),*(*last)(void)=symbol(image,"range_last");
 long (*middle)(void)=symbol(image,"range_middle");const uint64_t *data=symbol(image,"range_original_data");
 assert(first&&middle&&last&&data&&*data==UINT64_C(0x1020304050607080));
 assert(first()==pointer()&&last()==pointer());
 uintptr_t (*first_x18)(uintptr_t,uintptr_t)=symbol(image,"range_first_x18");
 uintptr_t (*middle_x18)(uintptr_t)=symbol(image,"range_middle_x18");
 uintptr_t (*last_x18)(uintptr_t,uintptr_t)=symbol(image,"range_last_x18");
 assert(first_x18&&middle_x18&&last_x18);
 assert(first_x18(100,0x1234)==((100+7)^0x1234));
 assert(middle_x18(2)==14&&middle_x18(1)==4);
 assert(middle_x18(0)==((uintptr_t)data&~(uintptr_t)4095));
 assert(last_x18(100,23)==118);
 const void *x18_entries[]={(const void *)first_x18,(const void *)middle_x18,(const void *)last_x18};
 for(size_t n=0;n<3;n++){
  uint32_t instruction=*(const uint32_t *)x18_entries[n];assert((instruction&0xfc000000u)==0x14000000u);
  int64_t offset=(int64_t)((int32_t)(instruction<<6)>>6)*4;
  read_execute_only((char *)x18_entries[n]+offset);
 }
 const uint32_t original_words[]={0xd53bd048,0xaa0003f2,0xd4000001};
 assert(!memcmp(symbol(image,"range_instruction_data"),original_words,sizeof original_words));
 uint32_t entry=((const uint32_t *)(void *)middle)[1];assert((entry&0xfc000000u)==0x14000000u);
 int64_t displacement=(int64_t)((int32_t)(entry<<6)>>6)*4;
 void *island=(char *)(void *)middle+4+displacement;
 read_execute_only(island);read_execute_only((void *)middle);
 long (*syscall)(long,long,long,long,long,long,long)=dlsym(core,"klb_syscall");assert(syscall&&middle()==syscall(178,0,0,0,0,0,0));
 assert(*data==UINT64_C(0x1020304050607080));unload(image);
 void *(*guest_open)(const char *,int)=dlsym(core,"klb_dlopen");
 const char *(*load_error)(void)=dlsym(core,"klj_image_load_error");
 const char *(*guest_error)(void)=dlsym(core,"klb_dlerror");
 assert(guest_open&&load_error&&guest_error&&!guest_open(argv[3],2));
 assert(strstr(load_error(),"1 unsupported TLS sites"));
 const char *short_error=guest_error();assert(short_error&&strlen(short_error)<128);
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
 int (*patch_x18)(void *,size_t,KljInlinePool *,size_t,void *,const klx_code_context *)=dlsym(core,"klj_x18_patch_pools_context");assert(patch_x18);
 uint32_t x18_words[]={0xaa0003f2,0x91001a52,0xd65f03c0};memcpy(code,x18_words,sizeof x18_words);
 uint64_t stats[32];errno=0;
 assert(patch_x18(code,sizeof x18_words,pools,2,stats,&context)<0&&errno==ERANGE);
 assert(!memcmp(code,x18_words,sizeof x18_words)&&!pools[0].used&&!pools[1].used);
 // A reachable but genuinely unsupported original x18 site remains a strict
 // transactional refusal; an earlier supported site must not be committed.
 KljInlinePool near={map+0x0820c000,page,0,0};
 memset(near.base,0xa5,page);
 uint32_t unsupported[]={0xaa0003f2,0xd63f0240,0xd65f03c0};memcpy(code,unsupported,sizeof unsupported);errno=0;
 assert(patch_x18(code,sizeof unsupported,&near,1,stats,&context)<0&&errno==ENOEXEC);
 assert(!memcmp(code,unsupported,sizeof unsupported)&&!near.used);
 for(size_t n=0;n<page;n++)assert(((const uint8_t *)near.base)[n]==0xa5);
 assert(!munmap(map,size));
 puts("PASS sparse large ELF executes original TP/SVC/x18 through outer and interior pools; original data and strictly refused plans remain unchanged");
 puts("PASS rejected original TP candidate retains its precise registry reason and bounded guest dlerror");
}
