.section .text.first,"ax",@progbits
.p2align 2
.global publication_reserve
.type publication_reserve,%function
publication_reserve:
mrs x0,tpidr_el0
ret
.size publication_reserve,.-publication_reserve
.macro slot name,section
.section \section,"ax",@progbits
.p2align 2
.global \name
.type \name,%function
\name:
.space 2048
.size \name,.-\name
.endm
slot publication_first,.text.first
slot publication_middle,.text.middle
slot publication_last,.text.last
.section .rodata,"a",@progbits
.global publication_data
.type publication_data,%object
publication_data:
.quad 0x1020304050607080
.size publication_data,8
.section .note.GNU-stack,"",@progbits
