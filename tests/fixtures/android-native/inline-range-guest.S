.macro function name, section
.section \section,"ax",@progbits
.p2align 2
.global \name
.type \name,%function
\name:
.endm
function range_first, .text.first
mrs x0,tpidr_el0
ret
.size range_first,.-range_first
function range_middle, .text.middle
mov x8,#178
svc #0
ret
.size range_middle,.-range_middle
function range_last, .text.last
mrs x0,tpidr_el0
ret
.size range_last,.-range_last
.section .rodata,"a",@progbits
.global range_original_data
.type range_original_data,%object
range_original_data:
.quad 0x1020304050607080
.size range_original_data,8
