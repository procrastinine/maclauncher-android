.text
.p2align 2
.global refused_entry
.type refused_entry,%function
refused_entry:
// An original function root cannot turn opaque instructions into executable
// evidence. The TP-shaped candidate must remain untouched and refuse loading.
.inst 0x5281807c
.inst 0x7a83a87d
.inst 0x2a85d475
.inst 0xd53bd053
ret
.size refused_entry,.-refused_entry
