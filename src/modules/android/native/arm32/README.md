# Android ARM32 execution component

This component loads little-endian ARM ELF32 shared objects and executes their
ARM and Thumb instructions through Dynarmic's AArch64 backend. A runtime containing
this component declares `arm32Version: 1`. APK preparation then automatically
selects `armeabi-v7a` or `armeabi` libraries when ARM64 libraries are unavailable;
ARM64 remains preferred. The JVM and AOSP framework remain 64-bit.

The execution path is ARM32 instructions → generated ARM64 code → the existing
ARM64 native services. This component adapts CPU execution, AAPCS32 calls, JNI
handles and pointer or structure widths. It does not implement another Android
platform. `NativeBinding`, class-loader ownership, the JVM, original AOSP APIs
and Klepton service lookup remain shared with ARM64 APKs. Native imports call
the same libc, GLES, audio, filesystem and other service owners through
`kl_shim_lookup`; only their ABI representations are translated.

Zlib stream initialization translates the original `sizeof(z_stream)` argument
with the stream record. Invalid caller sizes retain the original version-error
result. The existing representation owner retains each caller-owned stream's
native address through initialization, copy, streaming and reset calls until
the original End operation releases its state. Guest record storage and
allocator callbacks retain their original ownership.

The guest address space stores 32-bit virtual addresses independently of Mac
pointers. Borrowed native buffers reserve compact ABI-aligned ranges with their
exact accessible bounds; file and anonymous mappings retain Android page
alignment. Both use the same range and lifetime owner. Boost indexes free ranges
by aligned capacity and address for allocation, fixed mappings and released-range
coalescing. ELF REL, Android packed REL and RELR relocations resolve to guest
addresses or typed host call gates. Bionic's original packed relocation decoder
is reused. The common Klepton image registry owns dependency loading, symbol
lookup, constructor execution and retained initialization failures for both ELF
widths; the ARM32 component supplies only its image representation. Guest
permissions are checked on memory callbacks. Dynarmic's shared page table reads
and writes complete readable/writable pages directly. Compact edge pages refer
to the same reservation cells and original mapping metadata. Generated accesses
check their exact byte bounds and permissions before reading or writing native
storage. Permission lookup retains the checked byte offset for the native
access. Invalid accesses and accesses across page boundaries retain the original
bounded callbacks. Mapping,
permission and lifetime changes update that table through the same memory owner.
Permission failures use the original deferred callback branch range, including
large straight-line translated blocks.
Compact reservation cells use a sparse guest-page index derived from the owning
range map. Index entries borrow their original regions under the mapping guard;
large interior pages retain the direct table and original range lookup. A small
per-run cache covers hot working sets, while larger sets use the page index.
Removing a mapping clears its derived entries before releasing its owner.
Retiring executable storage invalidates its translated code before releasing the
mapping. Data-only buffer retirement updates mapping ownership and page lookup
without invalidating instruction caches. Executable publications and permission
changes invalidate earlier translations, including denied instruction fetches;
explicit instruction-cache flushes retain their requested ranges.
Guest call entry and return retain pending invalidations for Dynarmic's Run
owner while resetting execution halt reasons.
Translated code uses Dynarmic's original cache size and safe optimizations.
The original floating-point status manager retains the standard NEON control
mode across consecutive compatible operations. Scalar floating-point, memory
callbacks, control-register writes and block exits restore the original mode;
rounding, NaN handling, denormal handling and accumulated status are preserved.
Read-only native location queries use Dynarmic's original cache registration and
block maps to return a compiled block's guest descriptor and host extent.
Queries return an unavailable status while metadata changes and never execute
guest instructions.
Mach exception transport and protocol failures use the existing physical Android
log destination when supplied by the VM host. Accesses outside registered CPU
code return the original failure reply quietly to the next exception or signal
owner. Original CPU fastmem recovery and terminal fault diagnostics remain with
Dynarmic. Closing guest stderr cannot turn a diagnostic write into a C++ exception
or change the original Mach exception reply.
Native import failures also retain the current guest link register and r0–r3
without inspecting pointed-to contents. This context is read only on the
existing failure path; allocation bounds and original exception types remain
unchanged.
The unsigned execution budget is supplied to Dynarmic in positive signed
windows, retaining finite-budget exhaustion and the full remaining count.
Translated runs hold a shared mapping read guard until a native-call boundary;
native callbacks are retained under that guard and invoked after its release.
Allocation, protection and retirement take exclusive locks. Native interface
synchronization reads the representation buffers held by their existing owner.

AAPCS32 calls preserve register, floating-point and stack state across host
callbacks, including nested calls into guest code. Wide arguments use aligned
register pairs or the guest stack. CP15 thread-pointer reads use guest TLS.
The fixed Linux ARM helper page supplies its atomic operations, memory barrier
and TLS getter through the same translated CPU and shared memory owner.
Linux EABI SVC calls leave the translated execution loop before entering the
shared native imports. Their numbers and aligned register arguments are generated
from the pinned Bionic syscall declarations and original NDK Linux headers.
Arguments use r0–r6, the result replaces only r0, and failures return negative
Linux error numbers without changing libc errno. File, clock, mapping, signal
and futex operations retain their existing native owners; TLS and instruction
cache requests use the existing translated CPU and memory owners. Kernel-only
forms without a corresponding native ABI return `ENOSYS`.
The libc `syscall()` entry uses Bionic's original argument shift from the C
registers and stack into that same dispatcher. It converts kernel failures to
`-1` and the shared libc errno cell, preserving errno on success.
The original variadic `prctl` declaration uses its fixed five-word kernel ABI.
Name buffers are width-translated into the shared process-control owner; scalar
options retain their values, and unavailable Linux security controls retain the
same unsupported result as ARM64 calls.
Host calls similarly execute outside the JIT,
so JNI callbacks can reenter guest code without recursively running the
translator.

The syscall and import callbacks translate 32-bit guest pointers and structures
before calling the existing Klepton and Android native services. The optional JNI width adapter generates every table
slot from the selected JDK header. It preserves the original JVM object and
method owners, translates AAPCS32 argument representations, and uses system
libffi for registered native callbacks. Local reference frames and pinned array
and string buffers retain their original JNI lifetimes. Guest CPU state is
reused on each host thread and retired with that thread.

JNI libraries use the ordinary APK loader, constructors, dependency ordering and
registered methods. Native declarations are generated from the complete published
NDK headers for ARMv7 and AArch64, including both sides of their original API
level conditionals. Clang's canonical structure supplies pointer and array
layouts while original typedef ancestry preserves JNI reference semantics.
Anonymous structure and union storage remains part of that declaration graph.
Signal masks retain their original bits across Bionic's word and record layouts;
`sigaction` selects the declared handler signature from its flags and returns
the same guest callback identity through the shared signal owner.
Native function pointers returned to ARM32 receive typed executable import gates.
Calling them uses the same shared native implementation; passing them back retains
the original native address. Function pointers never become borrowed data pages.
EGL core and extension opaque values preserve native resource identities and
application tokens without reading resource bytes. Ordinary pointer buffers keep
their declared bounds and permissions; original EGL validates its handles.
OpenSL and OpenMAX interface-ID and requirement arrays use their declared
interface count for both exported functions and interface-table methods. The
same counted-span converter preserves null and empty inputs and validates the
complete guest array before widening its pointers.
Interface-query output types follow the original IID declarations. Scoped
relocation and symbol bindings retain their native IID provider for subsequent
queries, including UUID copies, without requiring global symbol visibility.
The returned interface uses its complete converted vtable and exact guest extent.
Fixed native arguments use a cached AAPCS register/stack layout and one bounded
span read. The ART binding transfers that span in one host operation; both VM
bindings validate stack memory through the same CPU memory owner. Native
signature initialization uses a standard once-only operation.
Graphics pointer plans retain their original native context-query callable with
the bound function. Buffer binding values are queried from the current context
for every invocation before selecting a guest pointer or buffer offset.
NDK imports translate JNI environments, virtual machines and reference tokens
through the same JNI owner before calling the existing ARM64 NDK service;
those tokens are never treated as guest pixel or structure buffers.
Native global data and legacy standard streams use the same
owners with translated pointer widths and array strides. Pointer and record
conversion, callback gates and native allocation lifetimes delegate to the
existing ARM64 operations. The common builtin ELF retains Bionic's original
ARM32 mutex, condition, read/write lock, barrier, spinlock, once and semaphore
implementations. Their inline state and static initializers keep the original
ABI; futex, clock and thread-identity operations enter the selected native owner.
Futex command and flag values come from the original Linux headers, including
priority-inheritance and requeue forms. Both VM bindings use the same kernel
leases for Android process and thread IDs, with separate physical Mac identities
for authenticated IPC and scheduling. Thread retirement releases its lease.
Socket authentication reads physical PID and UID credentials from the retained
native descriptor; Binder resolves the corresponding Android process lease.
Original Linux socket credentials continue to report Android identities.
Memory-copy, memory-fill and file-I/O byte spans address the ARM32 representation,
including converted pointer cells and interface tables. Their complete declared
length and access permissions apply before pointer conversion; empty spans do
not access memory. Writes to mutable representations are converted back before
native state is refreshed; typed interface arguments retain their original
native identity.
Allocator results retain the native allocator's fundamentally aligned addressable
extent, including padding within its final mapped alignment unit. The original
`malloc_usable_size` query remains unchanged. Explicit borrowed buffers and
converted records keep their declared bounds; allocation release retires aliases
across the same retained extent.
Thread-specific values remain opaque guest tokens in that owner's storage,
including values returned to destructor callbacks; storing them does not read
guest memory or require readable page permissions.
OpenGL string queries retain the context's original byte storage and expose the
complete terminated string, including Khronos's unsigned-byte representation.

Memory mapping uses the same native file and anonymous mapping services.
`mmap`, `mmap64` and the ARM32 page-offset entry `__mmap2` share that path.
The translated address space applies Android's 4 KiB page boundaries and
permissions, while native backing allocations use macOS page alignment.
Partial unmapping and fixed-address replacement retain the backing owner for
remaining guest ranges. Executable guest pages contain ARM32 instructions;
Dynarmic translates them without making those data pages executable on macOS.

Formatted C calls reuse Bionic's original argument-type parser and the selected
ARM64 formatter. Both AAPCS32 variadic arguments and explicit `va_list` values
are converted to the shared service's AAPCS64 representation; format annotations
and argument types come from the original headers. Width and pointer conversion
do not introduce a separate formatting or stream implementation. The common
formatter's format support applies to both CPU widths.

Loader address queries preserve the guest code address and delegate to the common
image registry, including normal absence for unmapped values. They do not turn
address metadata into readable-memory requests.
Pointer fields in output records retain their original bits until the native
owner replaces them; unused output fields do not become memory reads during
width conversion.

Native calls carry the original symbol identity through their ABI gate. An
invalid borrowed-buffer access reports its originating native symbol and mapped
extent alongside the guest address. Callback failures retain the first guest
fault, register values, original typed arguments and their mapped buffer origins.
An unresolved call reports its symbol when
invoked; unused imports do not prevent
loading. Framework, libc service, display and game-window ownership remain shared
with ARM64 execution.

The component build uses CMake, an Apple Silicon C++20 compiler and Boost headers:

JVM and ART native builds share the same generated import descriptors. Original
NDK AST extraction is reused independently from descriptor emission. The retained
descriptor inputs record the header supplier, Python interpreter and Khronos
registry; each component build refreshes conversion roles and counted pointer
metadata from those inputs. Unchanged descriptors preserve their file timestamps.
Direct CMake builds run the same refresh dependency before compiling imports.

```sh
node src/modules/android/native/arm32/build.mjs \
  --source src/external/dynarmic --output build/android-arm32 \
  --java-home /path/to/jdk \
  --bionic-source src/external/aosp-bionic \
  --elf-linker /path/to/rust-lld \
  --ndk-headers /path/to/ndk/sysroot/usr/include --python /path/to/python \
  --owner-header /path/to/bionic/libc/bionic/atexit.h
```

The normal Android build supplies the same arguments and the pinned Bionic
lifecycle declaration automatically. Bionic supplies the original ELF packed
decoder and private native data declarations. Omitting native declaration inputs builds
only the CPU and optional JNI component.

The header artifact is pinned in `ndk-headers.json`, including the full selected
tree hash. The developer header acquisition command is:

```sh
node src/modules/android/native/arm32/acquire-headers.mjs build/android-ndk-headers
```

`--ndk-headers` selects that artifact’s `include` directory. Acquisition downloads
original headers, the 223 KiB ARM compiler-rt archive and 58 KiB of
original native link metadata, without an NDK compiler. The link-only libraries
record ELF dependencies on the existing Android and logging providers; they
are not runtime implementations or packaged application libraries. Original
ARM ABI helpers are linked into a small ELF library and
loaded by the common image owner. Memory helpers call the existing native
services; arithmetic and emulated TLS retain the original compiler-rt algorithms.
The same guest-width image includes Bionic’s complete legacy NDK data module.
Its original character tables and pointer relocations remain ARM32 data, with no
duplicated character-conversion service or host pointers exposed to the guest.
It also includes Bionic's complete C++ allocation-operator unit. Original ARM32
symbol names and argument widths call the shared native allocator; ordinary,
array, nothrow and sized-delete operations do not acquire a separate heap.
Case-only Linux
header names are preserved through Clang's virtual filesystem on case-insensitive
Mac volumes. Original target constraints, such as LP32-only `time64.h`, remain
recorded for each target without removing the other target's declarations.

Optional GLES shader and texture diagnostics use the same native observer in
both execution backends. ART binds that observer to the original native-width
GLES entry before ARM32 argument conversion. With diagnostics disabled, imported
calls retain their original provider and perform no additional graphics queries.

The source revision is recorded in `source.json`. Dynarmic uses the 0BSD license;
its bundled dependencies retain their upstream notices. macOS executable memory
uses the upstream MAP_JIT and per-thread write-protection implementation.
