// Copied into an image-owned executable mapping by inline-syscall.c.
.text
.p2align 2
.globl _inline_jump_save
_inline_jump_save:
    adrp x16, _inline_jump_save_target@PAGE
    ldr x16, [x16, _inline_jump_save_target@PAGEOFF]
    br x16
.p2align 2
.globl _inline_tiny_stack
_inline_tiny_stack:
    stp x29, x30, [sp, #-32]!
    stp x19, x20, [sp, #16]
    mov x19, sp
    mov x16, x0
    mov sp, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    blr x16
    mov sp, x19
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
.p2align 2
.globl _inline_state_begin
.globl _inline_state_end
_inline_state_begin:
    sub sp, sp, #288
    stp q8, q9, [sp, #160]
    stp q10, q11, [sp, #192]
    stp q12, q13, [sp, #224]
    stp q14, q15, [sp, #256]
    stp x0, x30, [sp]
    str x30, [x0, #248]
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    str x29, [sp, #96]
    mrs x9, nzcv
    mrs x10, fpsr
    stp x9, x10, [sp, #112]
    mrs x9, fpcr
    str x9, [sp, #128]
    mov x9, sp
    str x9, [x0, #792]
    mov x9, #0xa0000000
    msr nzcv, x9
    mov x9, #1
    msr fpsr, x9
    mov x9, #0x1000000
    msr fpcr, x9
    mov x1, #257
    mov x2, #258
    mov x3, #259
    mov x4, #260
    mov x5, #261
    mov x6, #262
    mov x7, #263
    mov x8, #264
    mov x9, #265
    mov x10, #266
    mov x11, #267
    mov x12, #268
    mov x13, #269
    mov x14, #270
    mov x15, #271
    mov x16, #272
    mov x17, #273
    mov x19, #275
    mov x20, #276
    mov x21, #277
    mov x22, #278
    mov x23, #279
    mov x24, #280
    mov x25, #281
    mov x26, #282
    mov x27, #283
    mov x28, #284
    mov x29, #285
    movi v0.16b, #1
    movi v1.16b, #2
    movi v2.16b, #3
    movi v3.16b, #4
    movi v4.16b, #5
    movi v5.16b, #6
    movi v6.16b, #7
    movi v7.16b, #8
    movi v8.16b, #9
    movi v9.16b, #10
    movi v10.16b, #11
    movi v11.16b, #12
    movi v12.16b, #13
    movi v13.16b, #14
    movi v14.16b, #15
    movi v15.16b, #16
    movi v16.16b, #17
    movi v17.16b, #18
    movi v18.16b, #19
    movi v19.16b, #20
    movi v20.16b, #21
    movi v21.16b, #22
    movi v22.16b, #23
    movi v23.16b, #24
    movi v24.16b, #25
    movi v25.16b, #26
    movi v26.16b, #27
    movi v27.16b, #28
    movi v28.16b, #29
    movi v29.16b, #30
    movi v30.16b, #31
    movi v31.16b, #32
    mov x0, #33
    mov x8, #178
    svc #0
    str x30, [sp, #144]
    ldr x30, [sp]
    stp x0, x1, [x30, #0]
    stp x2, x3, [x30, #16]
    stp x4, x5, [x30, #32]
    stp x6, x7, [x30, #48]
    stp x8, x9, [x30, #64]
    stp x10, x11, [x30, #80]
    stp x12, x13, [x30, #96]
    stp x14, x15, [x30, #112]
    stp x16, x17, [x30, #128]
    stp x18, x19, [x30, #144]
    stp x20, x21, [x30, #160]
    stp x22, x23, [x30, #176]
    stp x24, x25, [x30, #192]
    stp x26, x27, [x30, #208]
    stp x28, x29, [x30, #224]
    ldr x9, [sp, #144]
    str x9, [x30, #240]
    stp q0, q1, [x30, #256]
    stp q2, q3, [x30, #288]
    stp q4, q5, [x30, #320]
    stp q6, q7, [x30, #352]
    stp q8, q9, [x30, #384]
    stp q10, q11, [x30, #416]
    stp q12, q13, [x30, #448]
    stp q14, q15, [x30, #480]
    stp q16, q17, [x30, #512]
    stp q18, q19, [x30, #544]
    stp q20, q21, [x30, #576]
    stp q22, q23, [x30, #608]
    stp q24, q25, [x30, #640]
    stp q26, q27, [x30, #672]
    stp q28, q29, [x30, #704]
    stp q30, q31, [x30, #736]
    mrs x9, nzcv
    str x9, [x30, #768]
    mrs x9, fpsr
    str x9, [x30, #776]
    mrs x9, fpcr
    str x9, [x30, #784]
    mov x9, sp
    str x9, [x30, #800]
    ldp x9, x10, [sp, #112]
    msr nzcv, x9
    msr fpsr, x10
    ldr x9, [sp, #128]
    msr fpcr, x9
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldr x29, [sp, #96]
    ldr x30, [sp, #8]
    ldp q8, q9, [sp, #160]
    ldp q10, q11, [sp, #192]
    ldp q12, q13, [sp, #224]
    ldp q14, q15, [sp, #256]
    add sp, sp, #288
    ret
_inline_state_end:
