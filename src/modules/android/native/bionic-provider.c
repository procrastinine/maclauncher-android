#include "bionic-provider.h"
#include <stdio.h>
#include <string.h>
static int invoke(const KljBionicProvider *provider,KljBionicLinkRequest *request,char *error,size_t capacity){
 if(!provider||!provider->enter||!provider->dispatcher){if(error&&capacity)snprintf(error,capacity,"Bionic call requires an initialized guest scope");return 0;}
 uint64_t status=provider->enter(provider->dispatcher,(uintptr_t)request);
 if(error&&capacity)snprintf(error,capacity,"%s",request->error);
 return status==0;
}
int klj_bionic_open(const KljBionicProvider *provider,const char *name,int flags,void **result,char *error,size_t capacity){
 if(!result)return 0;*result=0;KljBionicLinkRequest request={.operation=KLJ_BIONIC_OPEN,.text=(uintptr_t)name,.flags=(unsigned)flags};
 int ok=invoke(provider,&request,error,capacity);if(ok)*result=(void*)(uintptr_t)request.result;return ok;
}
int klj_bionic_symbol(const KljBionicProvider *provider,void *handle,const char *name,void **result,char *error,size_t capacity){
 if(!result)return 0;*result=0;KljBionicLinkRequest request={.operation=KLJ_BIONIC_SYMBOL,.handle=(uintptr_t)handle,.text=(uintptr_t)name};
 int ok=invoke(provider,&request,error,capacity);if(ok)*result=(void*)(uintptr_t)request.result;return ok;
}
int klj_bionic_close(const KljBionicProvider *provider,void *handle,char *error,size_t capacity){
 KljBionicLinkRequest request={.operation=KLJ_BIONIC_CLOSE,.handle=(uintptr_t)handle};return invoke(provider,&request,error,capacity);
}
