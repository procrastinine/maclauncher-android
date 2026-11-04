#include "startup.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){
 assert(argc==4);void *core=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);
 if(!core){fprintf(stderr,"%s\n",dlerror());return 2;}
 configure_startup(core,argc,argv);
 int (*configure)(const char *)=dlsym(core,"klj_process_ids_configure");
 void (*set_path)(const char *)=dlsym(core,"kl_set_library_path");
 void *(*open_image)(const char *,int)=dlsym(core,"klb_dlopen");
 void *(*symbol)(void *,const char *)=dlsym(core,"klb_dlsym");
 void *(*imports)(const char *)=dlsym(core,"kl_shim_lookup");
 const char *(*error)(void)=dlsym(core,"klb_dlerror");
 assert(configure&&set_path&&open_image&&symbol&&imports&&error&&configure(argv[3])>0);
 set_path(argv[2]);
 void *unrelated=open_image("libunrelated.so",2);assert(unrelated);
 int (*function)(void)=symbol(unrelated,"unrelated_value");assert(function&&function()==77);
 void *image=open_image("libroot.so",2);assert(image);
 function=symbol(image,"root_value");assert(function&&function()==47);
 function=symbol(image,"precedence");assert(function&&function()==101);
 function=symbol(image,"dependency_a");assert(function&&function()==21);
 function=symbol(image,"dependency_b");assert(function&&function()==22);
 function=symbol(image,"deep_value");assert(function&&function()==31);
 function=symbol(image,"breadth_precedence");assert(function&&function()==222);
 void *allocate=symbol(image,"malloc"),*release=symbol(image,"free");
 assert(allocate&&allocate==imports("malloc")&&release&&release==imports("free"));
 void *bytes=((void *(*)(size_t))allocate)(128);assert(bytes);memset(bytes,0x5a,128);((void (*)(void *))release)(bytes);
 /* Unrelated loaded images never enter the root's local dependency scope. */
 (void)error();assert(!symbol(image,"unrelated_value"));
 const char *failure=error();assert(failure&&strstr(failure,"undefined symbol: unrelated_value"));assert(!error());
 assert(!symbol(image,"hidden_value"));assert(error()&&!error());
 void *bare=open_image("libbare.so",2);assert(bare);
 function=symbol(bare,"bare_value");assert(function&&function()==17);
 assert(!symbol(bare,"malloc"));assert(error()&&!error());
 assert(!symbol(bare,"dependency_a"));assert(error()&&!error());
 puts("PASS named ELF handle own/dependency/BFS precedence, real libc adapters, unrelated/missing dependencies and dlerror");
}
