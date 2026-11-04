#include "startup.h"
#include <assert.h>
#include <dlfcn.h>
#include <locale.h>
#include <stdio.h>
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
 assert(configure&&set_path&&open_image&&symbol&&imports&&error&&configure(argv[3])>0);set_path(argv[2]);
 const char *names[]={"regcomp","regexec","regerror","regfree"};
 const char *owned[]={"klj_regex_regcomp","klj_regex_regexec","klj_regex_regerror","klj_regex_regfree"};
 for(unsigned i=0;i<4;i++)assert(imports(names[i])==dlsym(core,owned[i]));
 void *image=open_image("libregex-probe.so",2);if(!image){fprintf(stderr,"%s\n",error());return 3;}
 int (*probe)(void)=symbol(image,"regex_probe");assert(probe);
 const char *locales[]={"C","en_US.UTF-8"};
 for(unsigned i=0;i<2;i++){assert(setlocale(LC_CTYPE,locales[i]));int line=probe();if(line){fprintf(stderr,"regex fixture failed at line %d in %s\n",line,locales[i]);return 4;}}
 puts("PASS original-header ARM64 ELF regex: basic/extended, captures/backrefs, GNU dialect, locales/bytes, flags/bounds, errors and repeated free");
}
