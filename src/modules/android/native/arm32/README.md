# Android ARM32 execution component

This component loads little-endian ARM ELF32 shared objects and runs their ARM
and Thumb instructions through Dynarmic's AArch64 backend. A runtime that
contains this component declares `arm32Version: 1`. APK preparation then selects
`armeabi-v7a` or `armeabi` libraries automatically when no ARM64 libraries are
available; ARM64 stays preferred. The JVM and the AOSP framework remain 64-bit.

ARM32 instructions are translated to generated ARM64 code, which calls the
existing ARM64 native services. The component adapts CPU execution, AAPCS32
calls, JNI handles and pointer and structure widths; it does not implement a
second Android platform. `NativeBinding`, class-loader ownership, the JVM, the
original AOSP APIs and Klepton service lookup are shared with ARM64 APKs. Native
imports call the same libc, GLES, audio, filesystem and other service
implementations through `kl_shim_lookup`, and only their ABI representations
are translated.

## Address space and memory

Guest addresses are 32-bit virtual addresses, stored independently of Mac
pointers. Borrowed native buffers reserve compact, ABI-aligned ranges with their
exact accessible bounds, while file and anonymous mappings keep Android page
alignment; both use the same range and lifetime owner. Boost indexes free ranges
by aligned capacity and by address for allocation, fixed mappings and coalescing
of released ranges.

Guest permissions are checked on memory callbacks. Dynarmic's shared page table
reads and writes complete readable and writable pages directly. Compact edge
pages refer to the same reservation cells and original mapping metadata, and
generated accesses check their exact byte bounds and permissions before reading
or writing native storage. Permission lookup keeps the checked byte offset for
the native access. Invalid accesses and accesses that cross a page boundary go
through the original bounded callbacks. Mapping, permission and lifetime changes
update that table through the same memory owner. Permission failures use the
original deferred callback branch range, including in large straight-line
translated blocks.

Compact reservation cells use a sparse guest-page index derived from the owning
range map. Index entries borrow their original regions under the mapping guard,
while large interior pages keep the direct table and the original range lookup.
A small per-run cache covers hot working sets, and larger sets use the page
index. Removing a mapping clears its derived entries before its owner is
released.

Memory mapping uses the same native file and anonymous mapping services, and
`mmap`, `mmap64` and the ARM32 page-offset entry `__mmap2` share that path. The
translated address space applies Android's 4 KiB page boundaries and
permissions, while native backing allocations use macOS page alignment. Partial
unmapping and fixed-address replacement keep the backing owner for the remaining
guest ranges. Executable guest pages contain ARM32 instructions, which Dynarmic
translates without making those data pages executable on macOS.

Allocator results keep the native allocator's fundamentally aligned addressable
extent, including padding within its final mapped alignment unit, and the
original `malloc_usable_size` query is unchanged. Explicit borrowed buffers and
converted records keep their declared bounds, and releasing an allocation
removes aliases across the same retained extent.

Memory-copy, memory-fill and file-I/O byte spans address the ARM32
representation, including converted pointer cells and interface tables. Their
complete declared length and access permissions are checked before pointer
conversion, and empty spans do not access memory. Writes to mutable
representations are converted back before native state is refreshed, and typed
interface arguments keep their original native identity.

## ELF images

ELF REL, Android packed REL and RELR relocations resolve to guest addresses or
typed host call gates, and Bionic's original packed relocation decoder is
reused. The common Klepton image registry handles dependency loading, symbol
lookup, constructor execution and retained initialization failures for both ELF
widths; the ARM32 component supplies only its image representation.

Loader address queries keep the guest code address and delegate to the common
image registry, including the normal absence result for unmapped values. They
never turn address metadata into readable-memory requests.

## Translated code

Translated code uses Dynarmic's original cache size and safe optimizations.
Releasing executable storage invalidates its translated code before the mapping
is released, while releasing a data-only buffer updates mapping ownership and
page lookup without invalidating instruction caches. Executable publications and
permission changes invalidate earlier translations, including denied
instruction fetches, and explicit instruction-cache flushes keep their requested
ranges. Guest call entry and return keep pending invalidations for Dynarmic's
Run owner while resetting the execution halt reasons.

The original floating-point status manager keeps the standard NEON control mode
across consecutive compatible operations. Scalar floating-point, memory
callbacks, control-register writes and block exits restore the original mode,
and rounding, NaN handling, denormal handling and accumulated status are
preserved.

Read-only native location queries use Dynarmic's original cache registration
and block maps to return a compiled block's guest descriptor and host extent.
While metadata is changing, queries return an unavailable status, and they never
execute guest instructions.

The unsigned execution budget is passed to Dynarmic in positive signed windows,
which keeps finite-budget exhaustion and the full remaining count. Translated
runs hold a shared mapping read guard until a native-call boundary; native
callbacks are recorded under that guard and invoked after it is released.
Allocation, protection and release take exclusive locks. Native interface
synchronization reads the representation buffers held by their existing owner.

## Calls and system calls

AAPCS32 calls preserve register, floating-point and stack state across host
callbacks, including nested calls back into guest code. Wide arguments use
aligned register pairs or the guest stack. CP15 thread-pointer reads use guest
TLS. The fixed Linux ARM helper page supplies its atomic operations, memory
barrier and TLS getter through the same translated CPU and shared memory owner.
Host calls also run outside the JIT, so JNI callbacks can reenter guest code
without running the translator recursively.

Linux EABI SVC calls leave the translated execution loop before entering the
shared native imports. Their numbers and aligned register arguments are
generated from Bionic's syscall declarations and the original NDK
Linux headers. Arguments use r0–r6, the result replaces only r0, and failures
return negative Linux error numbers without changing libc errno. File, clock,
mapping, signal and futex operations keep their existing native implementations,
and TLS and instruction-cache requests use the existing translated CPU and
memory owners. Kernel-only forms without a corresponding native ABI return
`ENOSYS`.

The libc `syscall()` entry uses Bionic's original argument shift from the C
registers and stack into the same dispatcher. It converts kernel failures to
`-1` and the shared libc errno cell, and leaves errno unchanged on success.

The original variadic `prctl` declaration uses its fixed five-word kernel ABI.
Name buffers are width-translated for the shared process-control
implementation, scalar options keep their values, and unavailable Linux security
controls return the same unsupported result as ARM64 calls.

## Threads and synchronization

The component's common builtin ELF keeps Bionic's original ARM32 mutex, condition,
read/write lock, barrier, spinlock, once and semaphore implementations. Their
inline state and static initializers keep the original ABI, and futex, clock and
thread-identity operations enter the selected native implementation. Futex
command and flag values come from the original Linux headers, including
priority-inheritance and requeue forms.

Both VM bindings take Android process and thread IDs from the same leases, held
through kernel locks, while authenticated IPC and scheduling use the real Mac
process and thread IDs. A thread's lease is released when the thread shuts
down. Socket authentication reads the Mac PID and UID credentials from the
retained native descriptor, and Binder resolves the corresponding Android
process lease. Original Linux socket credentials still report Android IDs.

Thread-specific values stay opaque guest tokens in their owner's storage,
including values passed to destructor callbacks, and storing them neither reads
guest memory nor requires readable page permissions. Guest CPU state is reused
on each host thread and released with that thread.

## Native imports

Syscall and import callbacks translate 32-bit guest pointers and structures
before calling the existing Klepton and Android native services. Native
declarations are generated from the complete published NDK headers for ARMv7 and
AArch64, including both sides of their original API-level conditionals. Clang's
canonical structure supplies pointer and array layouts, while the original
typedef ancestry keeps JNI reference semantics, and anonymous structure and
union storage stays part of that declaration graph.

Fixed native arguments use a cached AAPCS register and stack layout and one
bounded span read. The ART binding transfers that span in one host operation,
and both VM bindings validate stack memory through the same CPU memory owner.
Native signature initialization uses a standard once-only operation.

Native function pointers returned to ARM32 code receive typed executable import
gates. Calling one uses the same shared native implementation, and passing it
back keeps the original native address. Function pointers never become borrowed
data pages.

Pointer fields in output records keep their original bits until the native
implementation replaces them, and unused output fields do not turn into memory
reads during width conversion.

Native global data and legacy standard streams use the same implementations with
translated pointer widths and array strides. Pointer and record conversion,
callback gates and native allocation lifetimes delegate to the existing ARM64
operations.

Formatted C calls reuse Bionic's original argument-type parser and the selected
ARM64 formatter. Both AAPCS32 variadic arguments and explicit `va_list` values
are converted to the shared service's AAPCS64 representation, with format
annotations and argument types from the original headers. Width and pointer
conversion add no separate formatting or stream implementation, so the common
formatter's format support applies to both CPU widths.

Zlib stream initialization translates the original `sizeof(z_stream)` argument
along with the stream record, and invalid caller sizes keep the original
version-error result. The existing representation owner keeps each caller-owned
stream's native address through initialization, copy, streaming and reset calls
until the original End operation releases its state. Guest record storage and
allocator callbacks keep their original ownership.

Signal masks keep their original bits across Bionic's word and record layouts.
`sigaction` selects the declared handler signature from its flags and returns
the same guest callback identity through the shared signal implementation.

### JNI

An optional JNI width adapter generates every table slot from the selected JDK
header. It keeps the original JVM object and method owners, translates AAPCS32
argument representations and uses system libffi for registered native
callbacks. Local reference frames and pinned array and string buffers keep their
original JNI lifetimes. JNI libraries use the ordinary APK loader, constructors,
dependency ordering and registered methods.

NDK imports translate JNI environments, virtual machines and reference tokens
through the same JNI implementation before calling the existing ARM64 NDK
service, and those tokens are never treated as guest pixel or structure buffers.

### Graphics and audio interfaces

EGL core and extension opaque values keep native resource identities and
application tokens without reading resource bytes. Ordinary pointer buffers keep
their declared bounds and permissions, and the original EGL validates its
handles. Graphics pointer plans keep the original native context-query callable
together with the bound function, and buffer binding values are queried from the
current context on every invocation before a guest pointer or buffer offset is
chosen. OpenGL string queries keep the context's original byte storage and
expose the complete terminated string, including Khronos's unsigned-byte
representation.

Optional GLES shader and texture diagnostics use the same native observer in
both execution backends. ART binds that observer to the original native-width
GLES entry before ARM32 argument conversion. With diagnostics disabled, imported
calls keep their original provider and make no additional graphics queries.

OpenSL and OpenMAX interface-ID and requirement arrays use their declared
interface count for both exported functions and interface-table methods. The
same counted-span converter keeps null and empty inputs and validates the
complete guest array before widening its pointers. Interface-query output types
follow the original IID declarations. Scoped relocation and symbol bindings keep
their native IID provider for later queries, including UUID copies, without
requiring global symbol visibility. The returned interface uses its complete
converted vtable and exact guest extent.

## Faults and diagnostics

Mach exception transport and protocol failures go to the existing physical
Android log destination when the VM host supplies one. Accesses outside
registered CPU code quietly return the original failure reply to the next
exception or signal owner. Original CPU fastmem recovery and terminal fault
diagnostics stay with Dynarmic. Closing guest stderr cannot turn a diagnostic
write into a C++ exception or change the original Mach exception reply.

Native import failures also record the current guest link register and r0–r3
without inspecting the memory they point to. This context is read only on the
existing failure path, and allocation bounds and original exception types are
unchanged.

Native calls carry the original symbol identity through their ABI gate. An
invalid borrowed-buffer access reports its originating native symbol and mapped
extent along with the guest address. Callback failures keep the first guest
fault, register values, original typed arguments and the mapped origins of their
buffers. An unresolved call reports its symbol when it is invoked, and unused
imports do not prevent loading. Framework, libc service, display and game-window
ownership stay shared with ARM64 execution.

## Building

Building the component needs CMake, an Apple Silicon C++20 compiler and Boost
headers. JVM and ART native builds share the same generated import descriptors,
and the original NDK AST extraction is reused independently of descriptor
emission. The retained descriptor inputs record the header source, Python
interpreter and Khronos registry, and each component build refreshes conversion
roles and counted pointer metadata from those inputs. Unchanged descriptors keep
their file timestamps. Direct CMake builds run the same refresh dependency
before compiling imports.

```sh
node src/modules/android/native/arm32/build.mjs \
  --source src/external/dynarmic --output build/android-arm32 \
  --java-home /path/to/jdk \
  --bionic-source src/external/aosp-bionic \
  --elf-linker /path/to/rust-lld \
  --ndk-headers /path/to/ndk/sysroot/usr/include --python /path/to/python \
  --owner-header /path/to/bionic/libc/bionic/atexit.h
```

The normal Android build supplies the same arguments and Bionic's lifecycle
declaration automatically. Bionic supplies the original ELF packed
decoder and private native data declarations. Without the native declaration
inputs, the build produces only the CPU and the optional JNI component.

### NDK headers

The NDK headers come as one artifact, recorded in `ndk-headers.json` with the
full selected tree hash. Developers acquire it with:

```sh
node src/modules/android/native/arm32/acquire-headers.mjs build/android-ndk-headers
```

`--ndk-headers` selects that artifact's `include` directory. Acquisition
downloads the original headers, the 223 KiB ARM compiler-rt archive and
58 KiB of original native link metadata, without an NDK compiler. The link-only
libraries record ELF dependencies on the existing Android and logging providers;
they are not runtime implementations or packaged application libraries.

Case-only Linux header names are preserved through Clang's virtual filesystem on
case-insensitive Mac volumes. Original target constraints, such as the LP32-only
`time64.h`, stay recorded for each target without removing the other target's
declarations.

### Guest-width runtime image

Original ARM ABI helpers are linked into a small ELF library that the common
image owner loads. Memory helpers call the existing native services, while
arithmetic and emulated TLS keep the original compiler-rt algorithms. The same
guest-width image includes Bionic's complete legacy NDK data module. Its
original character tables and pointer relocations stay ARM32 data, with no
duplicated character-conversion service and no host pointers exposed to the
guest. The image also includes Bionic's complete C++ allocation-operator unit:
original ARM32 symbol names and argument widths call the shared native
allocator, and ordinary, array, nothrow and sized-delete operations use no
separate heap.

## Source and license

This component's source revision is recorded in `source.json`. Dynarmic uses the 0BSD
license, and its bundled dependencies keep their upstream notices. macOS
executable memory uses the upstream MAP_JIT and per-thread write-protection
implementation.
