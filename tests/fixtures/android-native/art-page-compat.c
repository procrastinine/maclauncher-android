#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <link.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <android/dlext_private.h>

#ifdef COMPAT_LIBRARY
static int value=47;
#ifdef COMPAT_BTI
/* Keep the RELRO end on a physical page boundary: the original loader then
 * reads the actual GNU property note without a compatibility load-bias shift. */
__attribute__((used,section(".data.rel.ro"))) static const void *const relocation_probe[512]={&value};
#endif
int original_value(void){long result;__asm__ volatile("mov x18,%1\nadd %0,x18,#5":"=r"(result):"r"((long)value-5):"x18");return (int)result;}
#else
static int inspect(struct dl_phdr_info *info,size_t size,void *path){
 (void)size;
 if(!strstr(info->dlpi_name,"libcompat.so"))return 0;
 int fd=open(path,O_RDONLY);assert(fd>=0);
 for(int i=0;i<info->dlpi_phnum;i++){
  const ElfW(Phdr)*p=&info->dlpi_phdr[i];if(p->p_type!=PT_LOAD||(p->p_flags&(PF_W|PF_X)))continue;
  unsigned char file[16384];assert(p->p_filesz<=sizeof(file));assert(pread(fd,file,p->p_filesz,p->p_offset)==(ssize_t)p->p_filesz);
  const unsigned char *memory=(void*)(info->dlpi_addr+p->p_vaddr);unsigned differences=0;
  for(size_t at=0;at<p->p_filesz;at++)if(file[at]!=memory[at]){if(differences++<16)fprintf(stderr,"compat segment%d byte%zx original%02x mapped%02x\n",i,(size_t)p->p_offset+at,file[at],memory[at]);}
  fprintf(stderr,"compat segment%d differences=%u\n",i,differences);
  assert(differences==0);
 }
 close(fd);return 0;
}
int main(int argc,char **argv){
 assert(argc==2&&getpagesize()==16384);
 android_set_16kb_appcompat_mode(false);
 void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
 assert(!library);
 const char *error=dlerror();assert(error&&strstr(error,"program alignment (4096)"));
 android_set_16kb_appcompat_mode(true);
 library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
 if(!library){fprintf(stderr,"%s\n",dlerror());return 1;}
 assert(!(getauxval(AT_HWCAP2)&HWCAP2_BTI));
 dl_iterate_phdr(inspect,argv[1]);
 int (*value)(void)=dlsym(library,"original_value");
 if(!value){fprintf(stderr,"compat symbol: %s\n",dlerror());return 2;}
 int result=value();if(result!=47){fprintf(stderr,"compat value: %d\n",result);return 3;}
 pid_t child=fork();assert(child>=0);
 if(!child){assert(value()==47);assert(dlclose(library)==0);_exit(0);}
 int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 assert(dlclose(library)==0);
 puts("PASS original Bionic 4KB library rejection, compatibility load, relocation and execution on 16KB pages");
}
#endif
