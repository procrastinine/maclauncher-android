#include "darwin.h"
#include "../signal-bridge.h"
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

static libc_globals storage;
libc_globals* const klj_jump_globals=&storage;
void klj_jump_init_cookie(libc_globals*);
__attribute__((constructor)) static void initialize_cookie(){klj_jump_init_cookie(&storage);}

/* The original family also builds independently of a VM execution context. */
extern "C" __attribute__((weak)) void klj_executable_jump_save(uint64_t*){}
extern "C" __attribute__((weak)) void klj_executable_jump_restore(const uint64_t*){}
/* The same libplatform operation used by Darwin's own longjmp. Linux derives
 * SS_ONSTACK from the restored SP; Darwin also retains a kernel status bit. */
extern "C" void klj_jump_native_save(uint64_t* buffer){
  int saved=errno;stack_t stack={};
  buffer[25]=buffer[26]=buffer[27]=0;
  klj_executable_jump_save(buffer+25);
  buffer[28]=sigaltstack(nullptr,&stack)?0:(uint64_t)stack.ss_flags;
  errno=saved;
}
extern "C" void klj_jump_native_restore(const uint64_t* buffer){
  int saved=errno;
  klj_executable_jump_restore(buffer+25);
  constexpr unsigned set_alt_stack=0x40000000u,reset_alt_stack=0x80000000u;
  syscall(SYS_sigreturn,nullptr,buffer[28]&SS_ONSTACK?set_alt_stack:reset_alt_stack);
  errno=saved;
}

extern "C" int klj_jump_sigprocmask(int how,const uint64_t* mask,uint64_t* previous){
  int result=klj_signal_mask(how,mask,previous);
  if(result){errno=result;return -1;}return 0;
}
