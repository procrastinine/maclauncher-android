/* The real nativeabi.c also references its optional environment-only template.
 * This control never creates such a closure; an unexpected use aborts. */
.globl _klj_native_env_dispatch
_klj_native_env_dispatch:
 bl _abort
 brk #0
/* The original immutable nativeabi.S closure page protocol, unchanged. */
.p2align 14
.globl _klj_native_abi_page
_klj_native_abi_page:
.rept 1024
 adr x16,.-16384
 ldp x16,x17,[x16]
 br x17
 nop
.endr
