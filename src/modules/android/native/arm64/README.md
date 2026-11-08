# A64 CPU binding

This binding is built by the optional `ANDROID_A64_CPU` CMake target, which
uses the same Dynarmic dependency revision as the ARM32 CPU binding. `cpu.h` provides architectural registers,
bounded execution, cache invalidation and the binding's own stop operation.

The VM that runs the code supplies memory access, exclusive writes and counter
values, and the direct memory option uses that VM's native virtual addresses.
ELF loading, mapping permissions, Linux operations and thread lifetimes stay
with the caller.

Execution returns the original program counter and a typed exit for SVC,
memory errors, exceptions, unsupported instructions and instruction-cache
operations. The caller handles that exit through its existing platform
implementation. Original instruction bytes stay in guest memory, and Dynarmic
keeps its generated executable cache separately.

## Mutable code

ART builds enable ownership of original mutable code with
`prepareAimSource({..., mutableCode:true})`. This applies the existing
original-file adapter before the mutable-memory adapter in MacLauncher's staged
copy of the AIM source. The default preparation keeps its existing translator and ARM32
source configuration, and ordinary native ARM64 code still runs directly.

Guest instructions from owned mutable code run on the Unicorn-based precise CPU component through
`kl_a64_precise_run_owned`. A run holds its exact source mapping and VM lease
for at most 4096 instructions. Source writes, SVC, architectural exceptions and
range exits return to the existing owner before another instruction runs. EL0
VA-based cache clean and invalidate instructions use the original CPU access
checks with SCTLR_EL1.UCI enabled, matching Linux user space, and privileged
whole-cache operations keep their original exceptions. Changes to executable
source still go through the owner's existing publication and revalidation
steps. Raw native-writer fault continuations keep their original exclusive
interval behavior. Public views of original code stay read-only and
non-executable, and writes use the retained private backing.

Ordinary anonymous read-write data can continue within that bounded run when
Darwin reports a unique private object or untouched private zero-fill memory.
Executable, file-backed, shared and unclassified mappings keep their
source-write boundaries. Each VM lease renews this permission, and all mappings
are released before that lease ends. Mutable source backing uses a separate
application VM tag so that its retained aliases do not coalesce with adjacent
ordinary data; the tag itself grants no access or execution rights.

## Runner build

`buildArtRunner` accepts either the Unicorn checkout at its recorded revision or an existing
`buildPrecise` result with static archives and notices, and builds only the
Linux runner in the prepared AIM Cargo workspace. `buildArt({...,runner})`
assembles that runner and its upstream notices with the explicitly supplied
ART, framework, host and optional ARM32 components. The separate foreign-native
writer archive is compiled only when its source adapter is present.
