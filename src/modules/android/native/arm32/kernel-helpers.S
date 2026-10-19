// Linux ARM's fixed user-helper ABI. These are CPU operations executed by the
// same translator, with its shared memory/atomic owner; no native service is
// duplicated. Contract: Documentation/arch/arm/kernel_user_helpers.rst.
.syntax unified
.arm
.text
.org 0xf60
// int cmpxchg64(const int64_t *old, const int64_t *next, int64_t *address)
push {r1, r4-r7, lr}
ldrd r4, r5, [r0]
ldrd r6, r7, [r1]
dmb ish
1: ldrexd r0, r1, [r2]
eor r0, r0, r4
eor r1, r1, r5
orrs r0, r0, r1
bne 2f
strexd r0, r6, r7, [r2]
teq r0, #0
bne 1b
2: dmb ish
rsbs r0, r0, #0
pop {r1, r4-r7, pc}
.org 0xfa0
3: dmb ish
bx lr
.org 0xfc0
// int cmpxchg(int old, int next, int *address), including the carry result.
dmb ish
4: ldrex r3, [r2]
subs r3, r3, r0
strexeq r3, r1, [r2]
teqeq r3, #1
beq 4b
rsbs r0, r3, #0
b 3b
.org 0xfe0
mrc p15, 0, r0, c13, c0, 3
bx lr
.org 0xffc
.word 5
