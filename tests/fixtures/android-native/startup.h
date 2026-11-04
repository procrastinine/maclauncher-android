#include <assert.h>
#include <dlfcn.h>
#include "../../../src/modules/android/native/image-startup.h"

static void configure_startup(void *core,int argc,char **argv){
    int (*configure)(int,const char *const *,size_t,const char *const *)=dlsym(core,"klj_library_set_startup");
    const char *environment[]={NULL};
    assert(configure&&configure(argc,(const char *const *)argv,0,environment));
}
