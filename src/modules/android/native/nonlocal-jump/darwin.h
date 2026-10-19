#pragma once
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

struct libc_globals { long setjmp_cookie; };
extern libc_globals* const klj_jump_globals;
#define __libc_globals klj_jump_globals
#define __libc_init_setjmp_cookie klj_jump_init_cookie
#define __libc_safe_arc4random_buf arc4random_buf
#define __LIBC_HIDDEN__ __attribute__((visibility("hidden")))
#define __bionic_setjmp_cookie_get klj_jump___bionic_setjmp_cookie_get
#define __bionic_setjmp_cookie_check klj_jump___bionic_setjmp_cookie_check
#define __bionic_setjmp_checksum_mismatch klj_jump___bionic_setjmp_checksum_mismatch

// Original Bionic fatal endpoints retain bounded diagnostics and termination.
// No stdio stream lock can be inherited from the abandoned stack.
[[noreturn]] static inline void klj_jump_fatal(const char* format,...) {
  char text[256];va_list arguments;va_start(arguments,format);
  int length=vsnprintf(text,sizeof(text),format,arguments);va_end(arguments);
  if(length>0)(void)write(STDERR_FILENO,text,(size_t)length<sizeof(text)?(size_t)length:sizeof(text)-1);
  (void)write(STDERR_FILENO,"\n",1);abort();
}
#define async_safe_fatal klj_jump_fatal
