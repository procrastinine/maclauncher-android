#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <pthread.h>
#include <mach/mach.h>

/* Test-only observation of an ordinary native termination. No signal handler,
 * debugger or replacement runtime owner is involved. The original primitive
 * still performs shutdown (or skips it for _exit) with the original status. */
static int output=-1;
static int exit_enabled,io_enabled,rule_io_enabled;
static _Atomic unsigned io_records;
static _Atomic unsigned exit_records;
static _Atomic unsigned rule_io_records;
static _Atomic unsigned char rule_fds[8192];
static _Thread_local int reporting;
static int enabled(const char *name){const char *value=getenv(name);return value&&value[0]=='1'&&!value[1];}
__attribute__((constructor)) static void retain_output(void){
 exit_enabled=enabled("MACLAUNCHER_TEST_NATIVE_EXIT_TRACE");io_enabled=enabled("MACLAUNCHER_TEST_NATIVE_IO_TRACE");
 rule_io_enabled=enabled("MACLAUNCHER_TEST_NATIVE_WEBKIT_IO_TRACE");
 if(exit_enabled||io_enabled||rule_io_enabled)output=fcntl(STDERR_FILENO,F_DUPFD_CLOEXEC,3);
 if(rule_io_enabled&&output>=0)dprintf(output,"[android-test] WebKit rule io ready pid=%d\n",getpid());
}
/* Observe the physical stdio calls used by the guest-width import converter.
 * No stream/FD table or payload copy is needed: the FILE identity joins each
 * open, seek and read. A retained output survives original fclose(stderr).
 * Do not query the stream after an operation; another thread may retire it. */
static void report_io(const char *operation,const void *caller,const char *format,...){
 if(!io_enabled||output<0||reporting)return;
 int saved=errno;reporting=1;unsigned index=atomic_fetch_add_explicit(&io_records,1,memory_order_relaxed);
 if(index<4096){
  char line[1536];int used=snprintf(line,sizeof line,"[android-test] native io sequence=%u op=%s caller=%p ",index,operation,caller);
  va_list args;va_start(args,format);int count=vsnprintf(line+used,sizeof line-(size_t)used,format,args);va_end(args);
  if(count>0){size_t total=(size_t)used+(size_t)count;if(total>=sizeof line-1)total=sizeof line-2;line[total++]='\n';(void)write(output,line,total);}
 }else if(index==4096){const char message[]="[android-test] native io limit=4096\n";(void)write(output,message,sizeof message-1);}
 reporting=0;errno=saved;
}
static FILE *observed_fopen(const char *name,const char *mode){FILE *result=fopen(name,mode);report_io("fopen",__builtin_return_address(0),"path=\"%.768s\" mode=\"%.16s\" stream=%p errno=%d",name?name:"",mode?mode:"",result,errno);return result;}
static int observed_fseek(FILE *stream,long offset,int whence){int result=fseek(stream,offset,whence);report_io("fseek",__builtin_return_address(0),"stream=%p offset=%ld whence=%d result=%d errno=%d",stream,offset,whence,result,errno);return result;}
static int observed_fseeko(FILE *stream,off_t offset,int whence){int result=fseeko(stream,offset,whence);report_io("fseeko",__builtin_return_address(0),"stream=%p offset=%lld whence=%d result=%d errno=%d",stream,(long long)offset,whence,result,errno);return result;}
static size_t observed_fread(void *data,size_t size,size_t count,FILE *stream){size_t result=fread(data,size,count,stream);report_io("fread",__builtin_return_address(0),"stream=%p size=%zu count=%zu result=%zu errno=%d",stream,size,count,result,errno);return result;}
static int observed_fclose(FILE *stream){int result=fclose(stream);report_io("fclose",__builtin_return_address(0),"stream=%p result=%d errno=%d",stream,result,errno);return result;}
/* Content-rule compilation can be caught and hidden by application SDKs.
 * Observe only its own descriptor identities, never payload bytes or other
 * paths. Every primitive and its errno retain their original behavior. */
static int rule_descriptor(int fd){
 if(!rule_io_enabled||reporting||fd<0||fd>=8192)return 0;
 int saved=errno;char name[1024];int yes=atomic_load_explicit(&rule_fds[fd],memory_order_relaxed);
 if(fcntl(fd,F_GETPATH,name)==0){yes=strstr(name,"/ContentRuleList")!=NULL||strstr(name,"SerializedNFA")!=NULL;atomic_store_explicit(&rule_fds[fd],yes,memory_order_relaxed);}
 errno=saved;return yes;
}
static void report_rule_io(const char *operation,int fd,long long requested,long long result,int error,const void *caller){
 if(output<0||reporting)return;int saved=errno;reporting=1;
 unsigned index=atomic_fetch_add_explicit(&rule_io_records,1,memory_order_relaxed);
 if(index<128){Dl_info image={0};dladdr(caller,&image);char line[512];
  int length=snprintf(line,sizeof line,"[android-test] WebKit rule io sequence=%u op=%s fd=%d requested=%lld result=%lld errno=%d caller=%p image=%s+0x%lx\n",index,operation,fd,requested,result,error,caller,image.dli_fname?image.dli_fname:"?",image.dli_fbase?(unsigned long)((const char*)caller-(const char*)image.dli_fbase):0);
  if(length>0)(void)write(output,line,(size_t)length<sizeof line?(size_t)length:sizeof line-1);
 }reporting=0;errno=saved;
}
static ssize_t observed_write(int fd,const void *bytes,size_t length){if(!rule_io_enabled)return write(fd,bytes,length);int selected=rule_descriptor(fd);ssize_t result=write(fd,bytes,length);int saved=errno;if(selected)report_rule_io("write",fd,length,result,result<0?saved:0,__builtin_return_address(0));errno=saved;return result;}
static ssize_t observed_writev(int fd,const struct iovec *vectors,int count){if(!rule_io_enabled)return writev(fd,vectors,count);int selected=rule_descriptor(fd);ssize_t result=writev(fd,vectors,count);int saved=errno;if(selected)report_rule_io("writev",fd,count,result,result<0?saved:0,__builtin_return_address(0));errno=saved;return result;}
static ssize_t observed_pwrite(int fd,const void *bytes,size_t length,off_t offset){if(!rule_io_enabled)return pwrite(fd,bytes,length,offset);int selected=rule_descriptor(fd);ssize_t result=pwrite(fd,bytes,length,offset);int saved=errno;if(selected)report_rule_io("pwrite",fd,length,result,result<0?saved:0,__builtin_return_address(0));errno=saved;return result;}
static off_t observed_lseek(int fd,off_t offset,int whence){if(!rule_io_enabled)return lseek(fd,offset,whence);int selected=rule_descriptor(fd);off_t result=lseek(fd,offset,whence);int saved=errno;if(selected)report_rule_io("lseek",fd,offset,result,result<0?saved:0,__builtin_return_address(0));errno=saved;return result;}
static int observed_close(int fd){if(!rule_io_enabled)return close(fd);int selected=rule_descriptor(fd);int result=close(fd);int saved=errno;if(selected){report_rule_io("close",fd,0,result,result<0?saved:0,__builtin_return_address(0));atomic_store_explicit(&rule_fds[fd],0,memory_order_relaxed);}errno=saved;return result;}
static size_t observed_confstr(int name,char *buffer,size_t length){if(!rule_io_enabled)return confstr(name,buffer,length);size_t result=confstr(name,buffer,length);int saved=errno;if(name==_CS_DARWIN_USER_TEMP_DIR){const char *operation="confstr-temp";if(result>1&&buffer&&length>=result)operation=buffer[result-2]=='/'?"confstr-temp-slash":"confstr-temp-no-slash";report_rule_io(operation,-1,length,result,result?saved:errno,__builtin_return_address(0));}errno=saved;return result;}
static int observed_mkostemps(char *name,int suffix,int flags){if(!rule_io_enabled)return mkostemps(name,suffix,flags);int selected=name&&strstr(name,"SerializedNFA")!=NULL;int result=mkostemps(name,suffix,flags);int saved=errno;if(selected)report_rule_io("mkostemps-nfa",result,flags,result,result<0?saved:0,__builtin_return_address(0));errno=saved;return result;}
static int observed_mkstemps(char *name,int suffix){if(!rule_io_enabled)return mkstemps(name,suffix);int selected=name&&strstr(name,"SerializedNFA")!=NULL;int result=mkstemps(name,suffix);int saved=errno;if(selected)report_rule_io("mkstemps-nfa",result,suffix,result,result<0?saved:0,__builtin_return_address(0));errno=saved;return result;}
static void *observed_mmap(void *address,size_t length,int protections,int flags,int fd,off_t offset){if(!rule_io_enabled)return mmap(address,length,protections,flags,fd,offset);int selected=rule_descriptor(fd);void *result=mmap(address,length,protections,flags,fd,offset);int saved=errno;if(selected)report_rule_io("mmap",fd,length,result==MAP_FAILED?-1:0,result==MAP_FAILED?saved:0,__builtin_return_address(0));errno=saved;return result;}
static void report_exit(const char *kind,int status) {
 if(!exit_enabled||output<0||reporting)return;
 int saved=errno;reporting=1;
 if(atomic_fetch_add_explicit(&exit_records,1,memory_order_relaxed)>=16){reporting=0;errno=saved;return;}
 void *frames[96];int count=backtrace(frames,96);
 dprintf(output,"[android-test] native termination kind=%s status=%d pid=%d thread=%u\n",kind,status,getpid(),pthread_mach_thread_np(pthread_self()));
 for(int i=0;i<count;i++) {
  Dl_info symbol={0};(void)dladdr(frames[i],&symbol);
  dprintf(output,"[android-test] native exit #%d pc=%p image=%s+0x%lx symbol=%s+0x%lx\n",i,frames[i],
   symbol.dli_fname?symbol.dli_fname:"?",symbol.dli_fbase?(unsigned long)((char*)frames[i]-(char*)symbol.dli_fbase):0,
   symbol.dli_sname?symbol.dli_sname:"?",symbol.dli_saddr?(unsigned long)((char*)frames[i]-(char*)symbol.dli_saddr):0);
 }
 reporting=0;errno=saved;
}
__attribute__((noreturn)) static void observed_exit(int status){report_exit("exit",status);exit(status);}
__attribute__((noreturn)) static void observed_immediate_exit(int status){report_exit("_exit",status);_exit(status);}
__attribute__((noreturn)) static void observed_abort(void){int saved=errno;report_exit("abort",SIGABRT);errno=saved;abort();}
/* Linux raise/tgkill imports can deliver SIGABRT without calling abort().
 * Observe the original caller before dispatch, without installing a signal
 * handler or changing masks, targets, dispositions or errno semantics. */
extern int __pthread_kill(mach_port_t,int);
static int observed_raise(int signal){if(signal==SIGABRT)report_exit("raise-SIGABRT",signal);return raise(signal);}
static int observed_kill(pid_t pid,int signal){if(signal==SIGABRT&&pid==getpid())report_exit("kill-SIGABRT",signal);return kill(pid,signal);}
static int observed_pthread_kill(pthread_t thread,int signal){if(signal==SIGABRT)report_exit("pthread_kill-SIGABRT",signal);return pthread_kill(thread,signal);}
static int observed_thread_kill(mach_port_t thread,int signal){if(signal==SIGABRT&&pthread_from_mach_thread_np(thread))report_exit("__pthread_kill-SIGABRT",signal);return __pthread_kill(thread,signal);}
__attribute__((used)) static struct {const void *replacement,*original;} const interpose[]
 __attribute__((section("__DATA,__interpose")))={
  {(const void*)observed_exit,(const void*)exit},
  {(const void*)observed_immediate_exit,(const void*)_exit},
  {(const void*)observed_abort,(const void*)abort},
  {(const void*)observed_raise,(const void*)raise},
  {(const void*)observed_kill,(const void*)kill},
  {(const void*)observed_pthread_kill,(const void*)pthread_kill},
  {(const void*)observed_thread_kill,(const void*)__pthread_kill},
  {(const void*)observed_fopen,(const void*)fopen},
  {(const void*)observed_fseek,(const void*)fseek},
  {(const void*)observed_fseeko,(const void*)fseeko},
  {(const void*)observed_fread,(const void*)fread},
  {(const void*)observed_fclose,(const void*)fclose},
  {(const void*)observed_write,(const void*)write},
  {(const void*)observed_writev,(const void*)writev},
  {(const void*)observed_pwrite,(const void*)pwrite},
  {(const void*)observed_lseek,(const void*)lseek},
  {(const void*)observed_close,(const void*)close},
  {(const void*)observed_confstr,(const void*)confstr},
  {(const void*)observed_mkostemps,(const void*)mkostemps},
  {(const void*)observed_mkstemps,(const void*)mkstemps},
  {(const void*)observed_mmap,(const void*)mmap}
 };
