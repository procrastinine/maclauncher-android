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

The ART producer enables original mutable-code ownership with
`prepareAimSource({..., mutableCode:true})`. This applies the existing
original-file adapter before the mutable-memory adapter in the owned build
projection. The default preparation retains its existing translator and ARM32
source configuration. Ordinary native ARM64 code remains direct.

Enrolled guest instructions use the pinned Unicorn precise component through
`kl_a64_precise_run_owned`. A run retains its exact source mapping and VM
lease for at most 4096 instructions. Source writes, SVC, architectural exceptions
and range exits return to the existing owner before another instruction
executes. EL0 VA-based cache clean/invalidate instructions use the original CPU
access checks with SCTLR_EL1.UCI enabled, matching Linux userspace. Privileged
whole-cache operations retain their original exceptions. Executable source
changes still cross the existing owner publication and revalidation boundaries. Raw native-writer fault continuations retain their original exclusive
interval behavior. Public original-code views remain read-only and
non-executable, with writes using the retained private backing.

Ordinary anonymous read-write data can continue within that bounded run when
Darwin reports a unique private object or untouched private zero-fill memory.
Executable, file-backed, shared and unclassified mappings retain source-write
boundaries. Admission is renewed under each VM lease, and all mappings retire
before that lease ends. Mutable source backing uses a separate application VM
tag to prevent its retained aliases from coalescing with adjacent ordinary data;
the tag itself grants no access or execution authority.

`buildArtRunner` accepts either the pinned Unicorn checkout or an existing
`buildPrecise` result with static archives and notices. It builds only the
Linux runner in the prepared AIM Cargo workspace. `buildArt({...,runner})`
assembles that runner and its upstream notices with the explicitly supplied
ART, framework, host and optional ARM32 components. The separate foreign-native
writer archive is compiled only when its source adapter is present.
