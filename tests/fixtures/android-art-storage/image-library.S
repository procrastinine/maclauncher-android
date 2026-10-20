.text
.p2align 2
.global image_value
.type image_value,%function
image_value:
 mov x0,#42
 ret
.size image_value,.-image_value
.global image_thread_pointer
.type image_thread_pointer,%function
image_thread_pointer:
 mrs x0,tpidr_el0
 ret
.size image_thread_pointer,.-image_thread_pointer
