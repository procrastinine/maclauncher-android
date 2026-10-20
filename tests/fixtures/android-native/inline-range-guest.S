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
function range_first_x18, .text.first
mov x18,x0
add x18,x18,#7
eor x0,x18,x1
ret
.size range_first_x18,.-range_first_x18
function range_middle, .text.middle
mov x8,#178
svc #0
ret
.size range_middle,.-range_middle
function range_middle_x18, .text.middle
mov x18,x0
cbz x18,.Lzero
tbz x18,#1,.Leven
add x18,x18,#9
.Leven:
add x0,x18,#3
ret
.Lzero:
adrp x18,range_original_data
mov x0,x18
ret
.size range_middle_x18,.-range_middle_x18
function range_last, .text.last
mrs x0,tpidr_el0
ret
.size range_last,.-range_last
function range_last_x18, .text.last
mov x18,x0
sub x18,x18,#5
add x0,x18,x1
ret
.size range_last_x18,.-range_last_x18
.section .rodata,"a",@progbits
.global range_original_data
.protected range_original_data
.type range_original_data,%object
range_original_data:
.quad 0x1020304050607080
.size range_original_data,8
// Typed original data inside RX must never be patched or refused as code.
.section .text.first,"ax",@progbits
.global range_instruction_data
.type range_instruction_data,%object
range_instruction_data:
.inst 0xd53bd048
.inst 0xaa0003f2
.inst 0xd4000001
.size range_instruction_data,.-range_instruction_data
