.text
.p2align 2
.globl _publication_tp_begin
.globl _publication_tp_end
_publication_tp_begin:
stp x29,x30,[sp,#-16]!
mov x29,sp
mrs x0,tpidr_el0
mov x18,#0x1234
eor x0,x0,x18
ldp x29,x30,[sp],#16
ret
_publication_tp_end:
