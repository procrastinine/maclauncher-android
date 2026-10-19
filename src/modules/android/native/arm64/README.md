# A64 CPU binding

The optional `ANDROID_A64_CPU` CMake target uses the same pinned Dynarmic
dependency as the ARM32 CPU binding. `cpu.h` provides architectural registers,
bounded execution, cache invalidation and an owned stop operation.

The executing VM supplies memory access, exclusive writes and counter values.
The direct memory option uses that owner's native virtual addresses. ELF
loading, mapping permissions, Linux operations and thread lifetimes remain
with the caller.

Execution returns the original program counter and a typed exit for SVC,
memory errors, exceptions, unsupported instructions and instruction-cache
operations. The caller handles that exit through its existing platform
implementation. Original instruction bytes stay in guest memory; Dynarmic
owns the generated executable cache separately.
