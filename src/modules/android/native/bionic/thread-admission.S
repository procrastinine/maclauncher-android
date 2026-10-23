// Returning entry around original AIM resume; no Rust frame survives yield.
// Host caller remains on its actual physical JVM stack throughout the call.
.text
.p2align 2
.globl _klj_bionic_enter_context
_klj_bionic_enter_context:
 stp x29,x30,[sp,#-256]!
 stp x19,x20,[sp,#16]
 stp x21,x22,[sp,#32]
 stp x23,x24,[sp,#48]
 stp x25,x26,[sp,#64]
 stp x27,x28,[sp,#80]
 stp q8,q9,[sp,#96]
 stp q10,q11,[sp,#128]
 stp q12,q13,[sp,#160]
 stp q14,q15,[sp,#192]
 mrs x1,fpcr
 mrs x2,fpsr
 stp x1,x2,[sp,#224]
 mov x1,sp
 str x1,[x0,#{foreign_frame}]
 str x1,[x0,#{host_sp}]
 str xzr,[x0,#{foreign_reason}]
 b _linux_abi_resume
.p2align 2
.globl _klj_bionic_return_context
_klj_bionic_return_context:
 // x19 is the original GuestContext at the full dispatch return boundary.
 // All nested Rust dispatch frames have returned before this branch.
 ldr x0,[x19,#{foreign_reason}]
 ldr x1,[x19,#{foreign_frame}]
 mov sp,x1
 mov w1,#1
 str w1,[x19,#{in_host}]
 str xzr,[x19,#{foreign_frame}]
 ldp q8,q9,[sp,#96]
 ldp q10,q11,[sp,#128]
 ldp q12,q13,[sp,#160]
 ldp q14,q15,[sp,#192]
 ldp x1,x2,[sp,#224]
 msr fpcr,x1
 msr fpsr,x2
 ldp x19,x20,[sp,#16]
 ldp x21,x22,[sp,#32]
 ldp x23,x24,[sp,#48]
 ldp x25,x26,[sp,#64]
 ldp x27,x28,[sp,#80]
 ldp x29,x30,[sp],#256
 ret
