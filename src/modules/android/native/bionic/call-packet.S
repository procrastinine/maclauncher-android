#include "call-packet.h"
.text
.p2align 2
.global klj_call_packet_assembly
.type klj_call_packet_assembly,%function
klj_call_packet_assembly:
 stp x29,x30,[sp,#-48]!
 mov x29,sp
 stp x19,x20,[sp,#16]
 stp x21,x22,[sp,#32]
 mov x19,x0
 ldr x20,[x19,#KLJ_PACKET_STACK_BYTES]
 sub sp,sp,x20
 ldr x21,[x19,#KLJ_PACKET_STACK]
 mov x22,sp
 cbz x20,1f
0:
 ldr q16,[x21],#16
 str q16,[x22],#16
 subs x20,x20,#16
 b.ne 0b
1:
 ldp q0,q1,[x19,#KLJ_PACKET_V]
 ldp q2,q3,[x19,#KLJ_PACKET_V+32]
 ldp q4,q5,[x19,#KLJ_PACKET_V+64]
 ldp q6,q7,[x19,#KLJ_PACKET_V+96]
 ldr x16,[x19,#KLJ_PACKET_TARGET]
 ldp x0,x1,[x19,#KLJ_PACKET_X]
 ldp x2,x3,[x19,#KLJ_PACKET_X+16]
 ldp x4,x5,[x19,#KLJ_PACKET_X+32]
 ldp x6,x7,[x19,#KLJ_PACKET_X+48]
 ldr x8,[x19,#KLJ_PACKET_X+64]
 blr x16
 stp x0,x1,[x19,#KLJ_PACKET_RESULT_X]
 stp q0,q1,[x19,#KLJ_PACKET_RESULT_V]
 stp q2,q3,[x19,#KLJ_PACKET_RESULT_V+32]
 str xzr,[x19,#KLJ_PACKET_STATUS]
 mov sp,x29
 ldp x19,x20,[sp,#16]
 ldp x21,x22,[sp,#32]
 ldp x29,x30,[sp],#48
 ret
.size klj_call_packet_assembly,.-klj_call_packet_assembly
.section .note.GNU-stack,"",%progbits
