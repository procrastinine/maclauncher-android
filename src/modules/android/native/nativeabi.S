// Darwin JNI -> AAPCS64 stack repacking; shared immutable closure template.
.text
.p2align 2
.globl _klj_native_abi_dispatch
.private_extern _klj_native_abi_dispatch
_klj_native_abi_dispatch:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    // 64-byte invocation plus Darwin's 192-byte native continuation. Guest
    // pthread_exit returns here with ThreadDeath pending instead of retiring
    // the pthread underneath the VM's still-live JavaThread record.
    sub sp, sp, #256
    // Preserve the complete incoming register set while creating this thread's
    // Android JNIEnv. The real VM's environment and table are never replaced.
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x16, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    mov x1, x0
    sub x0, x29, #64
    bl _klj_jni_invocation_enter
    cbz w0, Lklj_native_abi_failed
    sub x0, x29, #256
    str x0, [x29, #-24]
    bl __setjmp
    cbnz w0, Lklj_native_abi_failed
    ldr x0, [sp, #0]
    bl _klj_guest_env
    cbz x0, Lklj_native_abi_failed
    ldr x1, [sp, #8]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x16, [sp, #64]
    ldp q0, q1, [sp, #80]
    ldp q2, q3, [sp, #112]
    ldp q4, q5, [sp, #144]
    ldp q6, q7, [sp, #176]
    add sp, sp, #224
    mov x9, x16
    ldr x17, [x9]
    ldrh w10, [x9, #8]
    ldrh w11, [x9, #10]
    sub sp, sp, x10
    add x12, x29, #16
    add x9, x9, #16
    mov x13, sp
    cbz w11, Lklj_native_abi_invoke
Lklj_native_abi_next:
    ldrh w14, [x9]
    ldrb w15, [x9, #2]
    add x14, x12, x14
    cmp w15, #8
    b.eq Lklj_native_abi_eight
    cmp w15, #4
    b.eq Lklj_native_abi_four
    cmp w15, #2
    b.eq Lklj_native_abi_two
    ldrb w14, [x14]
    b Lklj_native_abi_store
Lklj_native_abi_two:
    ldrh w14, [x14]
    b Lklj_native_abi_store
Lklj_native_abi_four:
    ldr w14, [x14]
    b Lklj_native_abi_store
Lklj_native_abi_eight:
    ldr x14, [x14]
Lklj_native_abi_store:
    str x14, [x13], #8
    add x9, x9, #4
    subs w11, w11, #1
    b.ne Lklj_native_abi_next
Lklj_native_abi_invoke:
    blr x17
Lklj_native_abi_return:
    sub sp, x29, #288
    stp x0, x1, [sp]
    str q0, [sp, #16]
    sub x0, x29, #64
    bl _klj_jni_invocation_leave
    ldp x0, x1, [sp]
    ldr q0, [sp, #16]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret

Lklj_native_abi_failed:
    mov x0, #0
    fmov d0, xzr
    b Lklj_native_abi_return

// An imported native NDK implementation borrows the actual JVM environment.
// Preserve all argument registers and the caller's original stack; this tail
// call changes only the declared JNIEnv argument, using the same closure owner.
.p2align 2
.globl _klj_native_env_dispatch
.private_extern _klj_native_env_dispatch
_klj_native_env_dispatch:
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x16, [sp, #64]
    stp x29, x30, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    ldr w9, [x16, #12]
    mov x10, sp
    ldr x0, [x10, x9, lsl #3]
    ldr x17, [x16, #1040]
    blr x17
    ldr x16, [sp, #72]
    ldr w9, [x16, #12]
    mov x10, sp
    str x0, [x10, x9, lsl #3]
    ldr x17, [x16]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldr x8, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    add sp, sp, #224
    br x17

// Each remapped instruction has its two-word record exactly one page below.
// PC-relative ADR stays valid at every mapping and touches no guest arguments.
.p2align 14
.globl _klj_native_abi_page
.private_extern _klj_native_abi_page
_klj_native_abi_page:
.rept 1024
    adr x16, . - 16384
    ldp x16, x17, [x16]
    br x17
    nop
.endr
