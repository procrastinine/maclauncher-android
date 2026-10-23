#include "bionic-provider.h"
#include <dlfcn.h>
#include <stdio.h>
/* Compiled as an ELF input. The existing host-call callback establishes the
 * real Bionic stack/TLS and Linux syscall owner before entering this function. */
uint64_t klj_bionic_link_dispatch(uint64_t address){
 KljBionicLinkRequest *request=(void*)(uintptr_t)address;
 if(!request)return 1;
 request->result=0;request->error[0]=0;(void)dlerror();
 switch(request->operation){
 case KLJ_BIONIC_OPEN:request->result=(uintptr_t)dlopen((const char*)(uintptr_t)request->text,(int)request->flags);break;
 case KLJ_BIONIC_SYMBOL:request->result=(uintptr_t)dlsym((void*)(uintptr_t)request->handle,(const char*)(uintptr_t)request->text);break;
 case KLJ_BIONIC_CLOSE:request->result=(uint64_t)(int64_t)dlclose((void*)(uintptr_t)request->handle);break;
 default:snprintf(request->error,sizeof request->error,"Invalid Bionic linker operation");return 1;
 }
 const char *error=dlerror();if(error){snprintf(request->error,sizeof request->error,"%s",error);return 1;}return 0;
}
