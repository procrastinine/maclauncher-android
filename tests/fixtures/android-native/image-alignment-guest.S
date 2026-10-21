.text
.global shifted_value
.type shifted_value,%function
shifted_value:
  mov w0,#42
  ret
.size shifted_value,.-shifted_value
.data
.global shifted_data
.type shifted_data,%object
shifted_data:
  .word 0x11223344
.size shifted_data,.-shifted_data
