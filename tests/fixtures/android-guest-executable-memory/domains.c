#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static size_t page;
static void *(*guest_map)(void*,size_t,int,int,int,off_t);
static int (*guest_protect)(void*,size_t,int),(*guest_unmap)(void*,size_t);
static void *(*guest_remap)(void*,size_t,size_t,int,...);
static vm_region_submap_info_data_64_t region(void *pointer){
 mach_vm_address_t at=(uintptr_t)pointer;mach_vm_size_t size=0;natural_t depth=0;
 vm_region_submap_info_data_64_t info={0};mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
 assert(!mach_vm_region_recurse(mach_task_self(),&at,&size,&depth,(vm_region_recurse_info_t)&info,&count));assert(at<=(uintptr_t)pointer&&size>(uintptr_t)pointer-at);return info;
}
static void foreign(void *address){
 vm_region_submap_info_data_64_t before=region(address);
 errno=0;int expected=mprotect(address,page,7),error=errno;
 if(!expected)assert(!mprotect(address,page,before.protection));
 errno=0;int actual=guest_protect(address,page,7),actual_error=errno;
 if(actual!=expected||(actual<0&&actual_error!=error))fprintf(stderr,"foreign address=%p native=%d/%d guest=%d/%d tag=%u object=%u\n",address,expected,error,actual,actual_error,before.user_tag,before.object_id);
 assert(actual==expected);if(actual<0)assert(actual_error==error);
 vm_region_submap_info_data_64_t after=region(address);
 assert(after.object_id==before.object_id&&after.user_tag==before.user_tag&&after.external_pager==before.external_pager&&after.share_mode==before.share_mode);
 if(!actual)assert(!mprotect(address,page,before.protection));
 else assert(after.protection==before.protection);
 // Darwin can refuse even unchanged MAP_JIT protections. Preserve that result
 // too, rather than assuming that the current bits imply a permitted call.
 errno=0;expected=mprotect(address,page,before.protection);error=errno;
 errno=0;actual=guest_protect(address,page,before.protection);actual_error=errno;
 assert(actual==expected);if(actual<0)assert(actual_error==error);
 assert(region(address).object_id==before.object_id);
}
static void code(void *pointer){const uint32_t instructions[]={0x52800540,0xd65f03c0};memcpy(pointer,instructions,sizeof instructions);}
static void execute(void *pointer){assert(!guest_protect(pointer,page,5));assert(((int(*)(void))pointer)()==42);}
static void ranges(void){
 unsigned char *first=guest_map(NULL,3*page,3,0x22,-1,0);assert(first!=MAP_FAILED);code(first);code(first+2*page);
 assert(!guest_unmap(first+page,page));
 void *native=mmap(first+page,page,3,MAP_FIXED|MAP_PRIVATE|MAP_ANON,-1,0);assert(native==first+page);foreign(native);
 assert(guest_map(first+page,page,3,0x32,-1,0)==first+page);code(first+page);execute(first+page);execute(first);execute(first+2*page);assert(!guest_unmap(first,3*page));
 // A neighboring live mapping forces MAYMOVE rather than in-place expansion.
 first=guest_map(NULL,2*page,3,0x22,-1,0);assert(first!=MAP_FAILED);code(first);
 void *moved=guest_remap(first,page,3*page,1);assert(moved!=MAP_FAILED&&moved!=first);execute(moved);assert(!guest_unmap(first+page,page));assert(!guest_protect(moved,3*page,3));
 assert(guest_remap(moved,3*page,page,0)==moved);execute(moved);
 pid_t child=fork();assert(child>=0);if(!child){assert(!guest_protect(moved,page,3));code(moved);execute(moved);assert(!guest_unmap(moved,page));_exit(0);}
 int status=0;pid_t ended=0;for(unsigned attempt=0;attempt<300&&!ended;attempt++){ended=waitpid(child,&status,WNOHANG);if(!ended)usleep(10000);}
 if(!ended){assert(!kill(child,SIGKILL));assert(waitpid(child,&status,0)==child);fprintf(stderr,"fork child did not finish; retired pid=%d\n",child);}
 assert(ended==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);assert(((int(*)(void))moved)()==42);assert(!guest_unmap(moved,page));
 // Original shared mremap aliases acquire the same guest mapping provenance.
 char name[]="/tmp/android-memory-domain-XXXXXX";int fd=mkstemp(name);assert(fd>=0&&!unlink(name)&&!ftruncate(fd,page));
 first=guest_map(NULL,page,3,1,fd,0);assert(first!=MAP_FAILED);code(first);
 void *alias=guest_remap(first,0,page,1);assert(alias!=MAP_FAILED);assert(!guest_unmap(first,page));assert(!close(fd));execute(alias);assert(!guest_unmap(alias,page));
 puts("PASS guest mmap, fixed replacement, partial retirement, mremap move/shrink/alias and fork retain executable ownership");
}
static void failed_fixed(void){
 void *first=guest_map(NULL,page,3,0x22,-1,0);assert(first!=MAP_FAILED);
 // Fail executable-context initialization after the physical fixed mapping
 // succeeds. Exhaustion belongs only to this short-lived subprocess.
 pthread_key_t keys[4096];size_t count=0;int error=0;
 while(count<4096&&!(error=pthread_key_create(&keys[count],NULL)))count++;
 assert(count<4096&&error==EAGAIN);
 assert(guest_map(first,page,7,0x32,-1,0)==MAP_FAILED);
 assert(mmap(first,page,3,MAP_FIXED|MAP_PRIVATE|MAP_ANON,-1,0)==first);
 foreign(first);assert(!munmap(first,page));
 while(count)assert(!pthread_key_delete(keys[--count]));
 puts("PASS failed executable fixed replacement retires prior guest ownership");
}
int main(int argc,char **argv){
 assert(argc==4);void *core=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);if(!core){fprintf(stderr,"%s\n",dlerror());return 2;}
 void *(*lookup)(const char*)=dlsym(core,"kl_shim_lookup");int (*configure)(const char*)=dlsym(core,"klj_process_ids_configure");assert(lookup&&configure&&configure(argv[2])>0);
 Dl_info image={0};char expected[PATH_MAX],loaded[PATH_MAX];assert(dladdr(lookup,&image)&&realpath(argv[1],expected)&&realpath(image.dli_fname,loaded)&&!strcmp(expected,loaded));
 guest_map=lookup("mmap");guest_protect=lookup("mprotect");guest_unmap=lookup("munmap");guest_remap=lookup("mremap");assert(guest_map&&guest_protect&&guest_unmap&&guest_remap);page=getpagesize();
 fprintf(stderr,"domain mode=%s\n",argv[3]);
 if(!strcmp(argv[3],"host-text"))foreign((void *)((uintptr_t)lookup("dlopen")&~(uintptr_t)(page-1)));
 else if(!strcmp(argv[3],"host-stack")){uintptr_t sp;__asm__ volatile("mov %0,sp":"=r"(sp));volatile uint64_t marker=0x123456789abcdef;foreign((void *)(sp&~(uintptr_t)(page-1)));assert(marker==0x123456789abcdef);}
 else if(!strcmp(argv[3],"host-jit")){void *jit=mmap(NULL,page,7,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0);assert(jit!=MAP_FAILED);foreign(jit);assert(!munmap(jit,page));}
 else if(!strcmp(argv[3],"failed-fixed")){failed_fixed();return 0;}
 else {assert(!strcmp(argv[3],"guest-ranges"));ranges();return 0;}
 printf("PASS %s retains original Darwin result, errno and mapping identity\n",argv[3]);
}
