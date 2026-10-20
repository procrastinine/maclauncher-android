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
