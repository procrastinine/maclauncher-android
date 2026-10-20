.text
.p2align 2
.global protected_entry
.type protected_entry,%function
protected_entry:
    // Original ELF metadata can name an opaque entry prefix. Its intact body
    // still uses the compiler's normal Bionic stack-protector sequence. The
    // probe enters the intact body directly; these words are never executed.
    .inst 0x5281807c
    .inst 0x7a83a87d
    .inst 0x2a85d475
    stp x19,x30,[sp,#-32]!
    mrs x19,tpidr_el0
    ldr x8,[x19,#40]
    mov x1,x0
    str x8,[sp,#16]
    mov x18,#0x456
    add x0,x1,x18
    ldr x8,[x19,#40]
    ldr x9,[sp,#16]
    cmp x8,x9
    b.ne .Lbad_guard
    ldp x19,x30,[sp],#32
    ret
.Lbad_guard:
    brk #0x702
.size protected_entry,.-protected_entry

.p2align 2
.global protected_spilled_entry
.type protected_spilled_entry,%function
protected_spilled_entry:
    .inst 0x5281807c
    .inst 0x7a83a87d
    .inst 0x2a85d475
    stp x29,x30,[sp,#-48]!
    stp x19,x20,[sp,#16]
    mov x29,sp
    sub sp,sp,#80
    mrs x8,tpidr_el0
    stur x8,[x29,#-64]
    ldr x8,[x8,#40]
    stur x8,[x29,#-8]
    mov x19,x0
    mov x18,#0x456
    add x0,x19,x18
    sub sp,sp,#32
    mov x8,#1
    ldur x8,[x29,#-64]
    ldr x8,[x8,#40]
    ldur x9,[x29,#-8]
    cmp w19,#0
    cset w10,eq
    cmp x8,x9
    b.ne .Lbad_spilled_guard
    mov sp,x29
    ldp x19,x20,[sp,#16]
    ldp x29,x30,[sp],#48
    ret
.Lbad_spilled_guard:
    brk #0x703
.size protected_spilled_entry,.-protected_spilled_entry

// Only the original compiler prologue is recognizable. Loading this original
// ELF must preserve every opaque continuation word; the probe never calls it.
.p2align 2
.global protected_opaque_continuation
.type protected_opaque_continuation,%function
protected_opaque_continuation:
    sub sp,sp,#128
    stp x30,x25,[sp,#64]
    stp x24,x23,[sp,#80]
    stp x22,x21,[sp,#96]
    stp x20,x19,[sp,#112]
    mrs x22,tpidr_el0
    ldr x8,[x22,#40]
    .inst 0x7a839c6b
    .inst 0x2a81fc7c
.size protected_opaque_continuation,.-protected_opaque_continuation

.p2align 2
.global protected_boundary
.type protected_boundary,%function
protected_boundary:
    ret
.size protected_boundary,.-protected_boundary

.section .rodata
.p2align 2
.global protected_constants
.type protected_constants,%object
protected_constants:
    .inst 0xd53bd053
    .inst 0xf9401668
    .inst 0xf9000be8
    .inst 0xd65f03c0
.size protected_constants,.-protected_constants
.section .note.GNU-stack,"",%progbits
