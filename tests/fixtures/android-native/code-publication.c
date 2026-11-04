#include "startup.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "image-pool-lease.h"
extern const uint8_t inline_state_begin[],inline_state_end[],publication_tp_begin[],publication_tp_end[];
void *inline_jump_save_target;
struct state {uint64_t registers[32];uint8_t vectors[512];uint64_t flags[3],before,after;};
static int permissions(void *pointer){
 mach_vm_address_t at=(uintptr_t)pointer;mach_vm_size_t size=0;vm_region_basic_info_data_64_t info;
 mach_msg_type_number_t n=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=0;
 assert(!mach_vm_region(mach_task_self(),&at,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&n,&object));
 if(object)mach_port_deallocate(mach_task_self(),object);
 return at==(uintptr_t)pointer||(uintptr_t)pointer>at?info.protection:0;
}
static uintptr_t branch_target(uint32_t *site){
 assert((*site&0xfc000000u)==0x14000000u);
 return (uintptr_t)site+(int64_t)((int32_t)(*site<<6)>>6)*4;
}
static void state_check(void *code,long expected){
 struct state state={0};errno=E2BIG;((void (*)(struct state *))code)(&state);assert(errno==E2BIG);
 assert(state.registers[0]==(uint64_t)expected);
 for(int i=1;i<=29;i++)if(i!=18)assert(state.registers[i]==(uint64_t)(i==8?178:0x100+i));
 assert(state.registers[30]==state.registers[31]&&state.before==state.after);
 for(int i=0;i<32;i++)for(int lane=0;lane<16;lane++)assert(state.vectors[i*16+lane]==i+1);
 assert(state.flags[0]==0xa0000000&&state.flags[1]==1&&state.flags[2]==0x1000000);
}
int main(int argc,char **argv){
 assert(argc==3||argc==4);int baseline=argc==4;
 void *core=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);if(!core){puts(dlerror());return 2;}
 configure_startup(core,argc,argv);
 int (*configure)(const char *)=dlsym(core,"klj_process_ids_configure");assert(configure&&configure(getenv("KL_ANDROID_ID_DIRECTORY"))>0);
 void (*enter)(void)=dlsym(core,"kl_thread_init");void *(*tp)(void)=dlsym(core,"klj_guest_thread_pointer");assert(enter&&tp);enter();
 void *(*load)(const char *)=dlsym(core,"kl_load_recursive"),*(*symbol)(void *,const char *)=dlsym(core,"kl_sym");
 const char *(*error)(void)=dlsym(core,"kl_error");void (*unload)(void *)=dlsym(core,"kl_unload");
 int (*protect)(void *,size_t,int)=dlsym(core,"klj_executable_protect");
 long (*syscall)(long,long,long,long,long,long,long)=dlsym(core,"klb_syscall");
 size_t (*leases)(uintptr_t,size_t,KljImagePoolLease *,size_t)=dlsym(core,"klj_image_gap_pool_leases");
 assert(load&&symbol&&error&&unload&&protect&&syscall);if(!baseline)assert(leases);
 void *image=load(argv[2]);if(!image){fprintf(stderr,"%s\n",error());return 3;}
 uint8_t *first=symbol(image,"publication_first"),*middle=symbol(image,"publication_middle"),*last=symbol(image,"publication_last");
 const uint64_t *data=symbol(image,"publication_data");assert(first&&middle&&last&&data&&*data==UINT64_C(0x1020304050607080));
 size_t page=getpagesize(),state_size=inline_state_end-inline_state_begin,tp_size=publication_tp_end-publication_tp_begin;
 assert(state_size<2048&&tp_size<2048);
 uintptr_t start=(uintptr_t)first&~(uintptr_t)(page-1),end=((uintptr_t)last+2048+page-1)&~(uintptr_t)(page-1);
 size_t size=end-start;assert(size>(1u<<28));
 assert(!protect((void *)start,size,PROT_READ|PROT_WRITE));
 memcpy(first,inline_state_begin,state_size);memcpy(middle,publication_tp_begin,tp_size);memcpy(last,inline_state_begin,state_size);
 if(baseline){errno=0;int result=protect((void *)start,size,PROT_READ|PROT_EXEC);fprintf(stderr,"BASELINE large code publication result=%d errno=%d\n",result,errno);return result?1:0;}
 KljImagePoolLease before[16],after[16];size_t count=leases(start,size,before,16);assert(count&&count<=16);
 // A valid first body followed by an unsupported original return through x18 must fail
 // before any requested mapping, source instruction or gap reservation changes.
 const uint32_t refused[]={0xa9bf7bfdu,0x910003fdu,0xaa0003f2u,0xd65f0240u,0xa8c17bfdu,0xd65f03c0u};
 memcpy(last,refused,sizeof refused);errno=0;
 assert(protect((void *)start,size,PROT_READ|PROT_EXEC)<0&&errno==ENOEXEC);
 assert(!memcmp(first,inline_state_begin,state_size)&&!memcmp(middle,publication_tp_begin,tp_size)&&!memcmp(last,refused,sizeof refused));
 assert(permissions(first)==3&&permissions(middle)==3&&permissions(last)==3);
 assert(leases(start,size,after,16)==count);for(size_t i=0;i<count;i++)assert(before[i].previous==after[i].previous);
 memcpy(last,inline_state_begin,state_size);
 assert(!protect((void *)start,size,PROT_READ|PROT_EXEC));
 assert(permissions(first)==5&&permissions(middle)==5&&permissions(last)==5);
 state_check(first,syscall(178,0,0,0,0,0,0));state_check(last,syscall(178,0,0,0,0,0,0));
 assert(((uintptr_t (*)(void))middle)()==((uintptr_t)tp()^0x1234));
 uintptr_t old_body=branch_target((uint32_t *)(middle+8));uint8_t retained[32];memcpy(retained,(void *)old_body,sizeof retained);assert(permissions((void *)old_body)==5);
 int borrowed=0;for(size_t i=0;i<count;i++)if(old_body>=(uintptr_t)before[i].pool.base&&old_body<(uintptr_t)before[i].pool.base+before[i].pool.size)borrowed=1;assert(borrowed);
 assert(*data==UINT64_C(0x1020304050607080));
 // Repeated decryption gets fresh evidence/new bodies; older live bodies keep
 // their bytes and executable permission until their source mapping retires.
 assert(!protect((void *)start,size,3));memcpy(first,inline_state_begin,state_size);memcpy(middle,publication_tp_begin,tp_size);memcpy(last,inline_state_begin,state_size);
 assert(!protect((void *)start,size,5));assert(!memcmp((void *)old_body,retained,sizeof retained));assert(permissions((void *)old_body)==5);
 assert(((uintptr_t (*)(void))middle)()==((uintptr_t)tp()^0x1234));state_check(last,syscall(178,0,0,0,0,0,0));
 assert(branch_target((uint32_t *)(middle+8))!=old_body);
 // PROT_NONE source contents remain readable only through the private COW
 // snapshot. Neither successful nor failed preparation changes them in place.
 void *(*lookup)(const char *)=dlsym(core,"kl_shim_lookup");assert(lookup);
 void *(*guest_map)(void *,size_t,int,int,int,off_t)=lookup("mmap");assert(guest_map);
 uint32_t *hidden=guest_map(NULL,page,3,0x22,-1,0);assert(hidden!=MAP_FAILED);memcpy(hidden,publication_tp_begin,tp_size);assert(!mprotect(hidden,page,0));
 assert(!protect(hidden,page,5));assert(((uintptr_t (*)(void))hidden)()==((uintptr_t)tp()^0x1234));
 void (*forget)(void *,size_t)=dlsym(core,"klj_executable_code_forget");assert(forget);forget(hidden,page);assert(!munmap(hidden,page));
 // The ordinary loader retains registry entries for the process lifetime.
 // Exercise the image owner's final retirement after the last registry lookup.
 unload(image);
 puts("PASS large decrypted publication retains original TP, SVC machine state, x18, source data, gap leases and repeated publication");
 puts("PASS whole-range refusal preserves original instructions, gap reservations and all requested mapping permissions");
}
