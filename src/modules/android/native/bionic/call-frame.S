#include "call-packet.h"
.text
.p2align 2
.globl _klj_bionic_native_abi_dispatch
_klj_bionic_native_abi_dispatch:
 stp x29,x30,[sp,#-16]!
 mov x29,sp
 sub sp,sp,#KLJ_PACKET_SIZE
 stp x0,x1,[sp,#KLJ_PACKET_X]
 stp x2,x3,[sp,#KLJ_PACKET_X+16]
 stp x4,x5,[sp,#KLJ_PACKET_X+32]
 stp x6,x7,[sp,#KLJ_PACKET_X+48]
 str x8,[sp,#KLJ_PACKET_X+64]
 stp q0,q1,[sp,#KLJ_PACKET_V]
 stp q2,q3,[sp,#KLJ_PACKET_V+32]
 stp q4,q5,[sp,#KLJ_PACKET_V+64]
 stp q6,q7,[sp,#KLJ_PACKET_V+96]
 mov x0,x16
 mov x1,sp
 add x2,x29,#16
 bl _klj_bionic_call_frame_dispatch
 ldp x0,x1,[sp,#KLJ_PACKET_RESULT_X]
 ldp q0,q1,[sp,#KLJ_PACKET_RESULT_V]
 ldp q2,q3,[sp,#KLJ_PACKET_RESULT_V+32]
 mov sp,x29
 ldp x29,x30,[sp],#16
 ret
