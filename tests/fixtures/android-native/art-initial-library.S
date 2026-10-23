.text
.p2align 14
.global fixture_value
.type fixture_value,%function
fixture_value:
    mov x9,x18
    mov x18,x0
    sub x0,x18,#224
    mov x18,x9
    ret
.size fixture_value,.-fixture_value
.p2align 14
.global fixture_native_read
.type fixture_native_read,%function
fixture_native_read:
    ldr w0,[x0]
    ret
.size fixture_native_read,.-fixture_native_read
.global fixture_native_value
.type fixture_native_value,%function
fixture_native_value:
    mov x0,#42
    ret
.size fixture_native_value,.-fixture_native_value
.section .note.GNU-stack,"",%progbits
