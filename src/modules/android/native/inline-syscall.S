// The site veneer owns x16/x30 and the return PC. Linux SVC preserves every
// other register, SIMD lane, status register and SP; only x0 receives a result.
// Go NOSPLIT stacks cannot host arbitrary C frames. The allocated Darwin key
// points into the existing Mach-thread execution owner and its guarded stack.
.text
.p2align 2
.globl _klj_inline_syscall_enter
_klj_inline_syscall_enter:
    .cfi_startproc
    // Only 64 bytes, including the site veneer, touch the guest stack.
    stp x14, x15, [sp, #-16]!
    stp x17, x30, [sp, #-16]!
    mrs x14, nzcv
    str x14, [sp, #-16]!
    mov x17, sp
    adrp x16, _klj_inline_call_slot_offset@PAGE
    ldr x16, [x16, _klj_inline_call_slot_offset@PAGEOFF]
    mrs x30, tpidrro_el0
    ldr x16, [x30, x16]
    cbz x16, 9f
    ldr x14, [x16]
    ldr x30, [x14]
    // A recursive host call already on this same owned stack stays below
    // the currently suspended C frame instead of resetting to its top.
    ldr x15, [x16, #8]
    cmp x17, x15
    b.lo 1f
    ldr x15, [x16, #16]
    cmp x17, x15
    b.hi 1f
    mov x30, x17
1:
    mov sp, x30
    sub sp, sp, #864
    str x16, [sp, #800]
    str x17, [sp, #808]
    ldr x15, [x14]
    str x15, [sp, #816]
    mov x15, sp
    str x15, [x14]
    ldr x14, [x16, #24]
    ldr w15, [x14]
    str w15, [sp, #824]
    mov w15, #1
    str w15, [x14]
    // CFA is the original guest SP, reached through the saved leaf frame.
    .cfi_escape 0x0f, 6, 0x8f, 0xa8, 0x06, 0x06, 0x23, 0x40
    .cfi_offset 30, -8
    .cfi_escape 0x10, 29, 3, 0x8f, 0xe8, 0x01
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    ldp x14, x15, [x17, #32]
    stp x14, x15, [sp, #112]
    ldr x14, [x17, #48]
    ldr x15, [x17, #16]
    stp x14, x15, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    ldr x30, [x17, #24]
    str x30, [sp, #240]
    ldr x14, [x17, #56]
    str x29, [sp, #832]
    str x14, [sp, #840]
    add x29, sp, #832
    stp q0, q1, [sp, #256]
    stp q2, q3, [sp, #288]
    stp q4, q5, [sp, #320]
    stp q6, q7, [sp, #352]
    stp q8, q9, [sp, #384]
    stp q10, q11, [sp, #416]
    stp q12, q13, [sp, #448]
    stp q14, q15, [sp, #480]
    stp q16, q17, [sp, #512]
    stp q18, q19, [sp, #544]
    stp q20, q21, [sp, #576]
    stp q22, q23, [sp, #608]
    stp q24, q25, [sp, #640]
    stp q26, q27, [sp, #672]
    stp q28, q29, [sp, #704]
    stp q30, q31, [sp, #736]
    ldr x9, [x17]
    mrs x10, fpsr
    str x9, [sp, #768]
    str x10, [sp, #776]
    mrs x9, fpcr
    str x9, [sp, #784]
    mov x0, sp
    bl _klj_inline_syscall_dispatch
    str x0, [sp]
    ldr x16, [sp, #800]
    ldr x14, [x16]
    ldr x15, [sp, #816]
    str x15, [x14]
    ldr x14, [x16, #24]
    ldr w15, [sp, #824]
    str w15, [x14]
    ldr x9, [sp, #768]
    ldr x10, [sp, #776]
    msr nzcv, x9
    msr fpsr, x10
    ldr x9, [sp, #784]
    msr fpcr, x9
    ldp q0, q1, [sp, #256]
    ldp q2, q3, [sp, #288]
    ldp q4, q5, [sp, #320]
    ldp q6, q7, [sp, #352]
    ldp q8, q9, [sp, #384]
    ldp q10, q11, [sp, #416]
    ldp q12, q13, [sp, #448]
    ldp q14, q15, [sp, #480]
    ldp q16, q17, [sp, #512]
    ldp q18, q19, [sp, #544]
    ldp q20, q21, [sp, #576]
    ldp q22, q23, [sp, #608]
    ldp q24, q25, [sp, #640]
    ldp q26, q27, [sp, #672]
    ldp q28, q29, [sp, #704]
    ldp q30, q31, [sp, #736]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x16, [sp, #808]
    mov sp, x16
    add sp, sp, #16
    ldp x17, x30, [sp], #16
    ldp x14, x15, [sp], #16
    .cfi_def_cfa sp, 16
    ret
9:
    // Native entries establish the thread owner before executable code runs.
    brk #0
    .cfi_endproc
