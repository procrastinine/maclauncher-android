#include <dlfcn.h>
#include <errno.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "jsig-bridge.h"
typedef int(*Action)(int,const struct sigaction*,struct sigaction*);
static _Atomic(Action) original_jsig;
static uintptr_t jsig_low,jsig_high;
static _Thread_local unsigned depth;
static _Atomic unsigned outer_calls,real_calls,rejected_recursion;
static void invalid(void){static const char text[]="jsig bridge: exact original library resolution failed\n";write(2,text,sizeof text-1);_exit(78);}
__attribute__((constructor))static void initialize(void){
 const char *path=getenv(KLJ_JSIG_LIBRARY_ENV);char canonical[4096];struct stat file={0};
 if(!path||path[0]!='/'||!realpath(path,canonical)||strcmp(path,canonical)||lstat(path,&file)||!S_ISREG(file.st_mode))invalid();
 size_t length=strlen(path);static const char suffix[]="/lib/libjsig.dylib";if(length<sizeof suffix||strcmp(path+length-(sizeof suffix-1),suffix))invalid();
 // The launcher supplies the exact already-verified selected JDK file. Never
 // search for or load another implementation as a fallback.
 void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);if(!library)invalid();
 Action function=(Action)dlsym(library,"sigaction");Dl_info owner={0};if(!function||!dladdr((void*)function,&owner)||!owner.dli_fname||strcmp(owner.dli_fname,path))invalid();
 const char *hooks[]={"JVM_begin_signal_setting","JVM_end_signal_setting","JVM_get_signal_action"};for(unsigned n=0;n<3;n++){void *value=dlsym(RTLD_DEFAULT,hooks[n]);Dl_info hook={0};if(!value||!dladdr(value,&hook)||!hook.dli_fname||strcmp(hook.dli_fname,path))invalid();}
 const struct mach_header_64 *header=owner.dli_fbase;if(!header||header->magic!=MH_MAGIC_64||header->sizeofcmds>(1u<<20))invalid();
 const char *cursor=(const char*)(header+1),*end=cursor+header->sizeofcmds;for(uint32_t n=0;n<header->ncmds;n++){if((size_t)(end-cursor)<sizeof(struct load_command))invalid();const struct load_command *command=(const void*)cursor;if(command->cmdsize<sizeof(*command)||(size_t)(end-cursor)<command->cmdsize)invalid();if(command->cmd==LC_SEGMENT_64){if(command->cmdsize<sizeof(struct segment_command_64))invalid();const struct segment_command_64 *segment=(const void*)command;if(!strcmp(segment->segname,"__TEXT")){if(segment->fileoff||segment->vmsize>UINTPTR_MAX-(uintptr_t)header)invalid();jsig_low=(uintptr_t)header;jsig_high=jsig_low+segment->vmsize;}}cursor+=command->cmdsize;}
 if(!jsig_low||jsig_high<=jsig_low)invalid();atomic_store_explicit(&original_jsig,function,memory_order_release);
 fprintf(stderr,"jsig bridge: exact original sigaction=%s; caller bounds retained; no flat namespace\n",owner.dli_fname);
}
static int bridge(int signal,const struct sigaction *action,struct sigaction *previous){
 Action function=atomic_load_explicit(&original_jsig,memory_order_acquire);if(!function)return sigaction(signal,action,previous);
 if(depth){uintptr_t caller=(uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));if(depth!=1||caller<jsig_low||caller>=jsig_high){atomic_fetch_add_explicit(&rejected_recursion,1,memory_order_relaxed);errno=EDEADLK;return -1;}
  atomic_fetch_add_explicit(&real_calls,1,memory_order_relaxed);
  // dyld exempts this image's own libc reference from its interposition. The
  // original jsig implementation can therefore call its real OS entry even
  // when dyld interposes the pointer returned by jsig's RTLD_NEXT lookup.
  return sigaction(signal,action,previous);
 }
 depth=1;atomic_fetch_add_explicit(&outer_calls,1,memory_order_relaxed);int result=function(signal,action,previous);depth=0;return result;
}
static Action original_action(void){return sigaction;}
static void counts(KljJsigCounts *output){*output=(KljJsigCounts){sizeof(*output),KLJ_JSIG_BRIDGE_ABI,atomic_load_explicit(&outer_calls,memory_order_relaxed),atomic_load_explicit(&real_calls,memory_order_relaxed),atomic_load_explicit(&rejected_recursion,memory_order_relaxed)};}
const KljJsigBridgeApi *klj_jsig_bridge_get_api(void){static const KljJsigBridgeApi api={sizeof(api),KLJ_JSIG_BRIDGE_ABI,original_action,counts};return &api;}
__attribute__((used,section("__DATA,__interpose")))static const struct{const void *replacement,*original;}mapping={(const void*)bridge,(const void*)sigaction};
