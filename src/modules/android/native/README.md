# Android native host

MacLauncher's Android native host runs Android applications. Its JVM backend
combines Java 21, the complete Android 16 Java framework, dex2jvm and Klepton
native code. ARM32 libraries run on Dynarmic and use the same platform
services as ARM64. The
[Android runtime architecture](../../../../docs/runtimes/ANDROID_ARCHITECTURE.md)
describes execution, platform boundaries, resource ownership and host policy,
and [Android native code](../../../../docs/runtimes/ANDROID_NATIVE_CODE.md)
covers native library loading and instruction adaptation.

## Source layout

| Sources | Responsibility |
| --- | --- |
| `java/maclauncher/android` | Application entry, original framework bootstrap, package metadata and process authority |
| `framework`, `converter`, `portability` | Framework assembly and exact JVM/native dependency adaptations |
| `nativebinding.c`, `loader-*.c`, `nativeabi.*`, `ndk-jni` | Native loading, original JNI declarations and Android/Darwin calling conventions |
| `arm32` | ARM/Thumb execution, ELF32 records, AAPCS32 and JNI width conversion |
| `bionic-provider.*`, `bionic-provider-guest.c` | Synchronous original Bionic linker operations through the existing guest callback |
| `binder-rpc`, `activity-thread`, `keystore` | Shared Binder transport, application lifecycle and profile services (Android system services shared by all applications) |
| `native-*.mm`, `window*`, `presentation.mm`, `clock` | AppKit, input, display, surfaces and frame scheduling |
| `gles-guest-abi.*` | Original GLES declaration types and AAPCS64-to-Darwin stack adapters |
| `media-foundation`, `audio*`, `video*` | Original media-format algorithms and native playback/device operations |
| `libcore`, `conscrypt`, `process-abi`, `native-clock` | Original portable libraries and precise Darwin ABI adaptation |
| `file-events`, `pollable-fds` | Shared file-descriptor lifetime, polling, counters, timers and directory state |
| `fonts`, `system-config.mjs` | Android fonts and platform configuration |
| `upstream` | Source export, original Blueprint parsing, compiler inputs and incremental reuse utilities |

The `native-regex` component supplies `regcomp`, `regexec`, `regerror` and
`regfree` from Bionic's complete NetBSD regex implementation. It keeps Android's
public records, basic and extended expressions, capture offsets, backreferences
and `REG_GNU` syntax. Character classification and locale values come from the
existing native libc implementation, and allocation uses the physical heap.

## Sources and framework assembly

Each source catalog records the original repository and the exact revision.
Original checkouts stay unchanged, and MacLauncher's adaptations are applied to
the exported compiler inputs. The complete supplied framework keeps its class
and method surface, and the original Apache HTTP and StatsD modules are compiled
from their complete published source groups against that framework. The JNI
registration families that the supplied native library publishes are enumerated
from it and kept whenever their final framework classes still have native
declarations; the build does not
select registrations by package or class-name conventions.

The framework binary input directory also contains the recorded original
SettingsProvider APK. The build verifies its checksum and manifest, includes
the unchanged APK and resources under `platform/packages/`, and writes the
original package metadata to `platform/packages.json`. The component records its source and the system image it came from
separately. The image's
other packages are not part of this platform component collection.

DEX array-data payloads keep their primitive element type and raw values. When
an ordinary method's translation exceeds the JVM bytecode limit, conversion can
move its array stores into size-limited private static helpers. The original
method keeps its declaration, array allocation, store order, exception handlers
and monitor ownership, and null and bounds failures keep the element writes that
preceded them. Methods that fit keep their ordinary bytecode.

## Framework integration

Both VM entries publish Android's original process font map before the
application UI is built; an existing map keeps its Typeface objects and backing
memory.

Presentation cleanup uses the shared task manager across Activity navigation and
configuration relaunches. It shuts down the current Activity stack on the
manager's thread before releasing view roots, display callbacks and the native
output.

SurfaceControl transaction completion follows the frame that the existing output
consumed. ART output requires observed presentation API 1 and submits frames
without a presentation target, keeping the source timestamp, producer token and
display retirement callback through configured frame generation. Native output
uses the common presenter's observed display retirement, and dropped or merely
encoded images do not report completion. TransactionStats keeps the original
latch timestamp API and an empty present fence when that output exports no
Android fence descriptor.

Package labels and icons come from the original parser and resource renderer in
a headless metadata process, whose batch and idle lifetime is separate from game
activities. Cache keys include the package content, locale and metadata format.
Game processes use a private application bundle with the selected JDK, package
name and rendered icon, and headless profile services use accessory application
behavior.

## Native loading and JNI

Android native libraries receive a JNI environment facade, while the real Java
objects stay owned by the JVM. Android or Darwin argument layouts are chosen from
the calling code's registered image. Per-thread JNI state and attachments are
released when the corresponding thread exits, including callbacks that the host
created. Native library state and class references keep their library's load
lifetime.

The selected Java VM image comes before the JVM process adapter in the native
preload list. The adapter interposes the Invocation API directly and keeps
shutdown hooks, VM-death notifications and an existing exit callback before
process shutdown. Original dyld symbol scopes, dependency search, interposed
providers and lookup errors are unchanged. If the VM image is missing, native
process initialization stops.

The Bionic linker component runs `dlopen`, `dlsym` and `dlclose` on an already
initialized Bionic thread through the existing host-call callback. The caller
keeps the callback frame, linker namespace and thread lifetime, and handles and
symbol addresses stay with the original ELF linker. The component's flags use
Android's `RTLD_*` values, which differ numerically from Darwin's loader flags.

Raw ARM64 imports search images marked `DF_1_GLOBAL` and the requesting image's
retained `DT_NEEDED` dependencies, recursively. Explicit `RTLD_GLOBAL` controls dynamic symbol
visibility separately, and reopening an existing image keeps its original flags.
Named handles traverse dependencies in breadth-first order. Default and next
lookups keep the native caller and the original target-SDK compatibility, and
the application target is fixed before native images are published. Raw ARM64
lookup uses each mapped image as its own local root, while format adapters keep
the roots that their group loader assigned.

Native ABI metadata lookup keeps the calling library's class-loader namespace.
The configured application ABI selects ELF dependencies and system library paths
before native constructors run, and ARM32 and ARM64 imports use the same
platform service providers and per-library class-loader ownership. Native load
diagnostics keep the registry's rejection reason, while guest `dlerror` strings
keep their bounded Android form.

## ARM64 code adaptation and TLS

Large ARM64 ELF images reserve instruction veneers at both ends of their mapping
and use the original gaps between load segments for sites outside both branch
windows. TP, Linux SVC and original x18 bodies share one placement transaction,
which uses the original decoder and unmodified instruction neighborhoods. Every
entry, return and relocated conditional branch is checked before any instruction
changes. Original segment bytes keep their addresses, packed veneer pages contain
no writable original segment, and used pools end up read-only and executable.
Layouts without enough reachable storage report a loader error.

Later executable publication works on a private snapshot of the complete
requested range, including protected pages. Newly decoded instructions use the
same placement planner and can use unused reserved ELF gaps within that range.
All checks finish before one final mapping replacement, and a refused
publication keeps the original bytes and permissions. Published instruction
bodies keep their storage until the corresponding code mapping is removed.

Original typed data stays unchanged and is counted separately from uncertain TP
candidates, which still cause the load to be refused. Complete paired
stack-canary sequences can establish executable continuations through matching
SP or FP slots, and fixed FP slots can hold TP across register reuse and dynamic
SP allocation. Guard comparisons keep both original values through a limited
number of intervening calculations. A rooted, aligned prologue that preserves LR
and its callee-saved TP register can establish its own exact TP and canary-load
prefix before an opaque continuation, and that continuation gains no evidence
from the prefix. Opaque prefixes and mismatched canary slots supply no such
evidence.

ARM64 ELF TLS supports `PT_TLS`, `DTPMOD64`, `DTPREL64`, `TPREL64` and `TLSDESC`
while keeping the real Android thread pointer and the original Bionic slots.
Initial-exec and dynamic references share the same per-thread address. A private
demand-zero reserve provides one MiB of fixed TLS layout with alignment up to
64 KiB, and module offsets stay reserved after unloading. New modules initialize
existing physical threads before their native entry points become available.
Modules that do not fit that layout use dynamic TLS, and an initial-exec
reference to one of them reports a loader error instead of exposing an unrelated
address.

## Graphics

A generated guest library, the Bionic graphics carrier, forwards native-window
calls to the host and keeps the complete original native-window API family. Java Surface conversion, native reference counts, geometry and EGL
producers share the same host-owned window, and opaque host handles never enter
the image's Android Surface implementation. The window supports identity buffer
transforms and the default unknown dataspace. Other buffer transforms, display
refresh selection and native Parcel transfer return Android native errors.
Clearing a refresh preference keeps the system default, and preallocation is a
best-effort hint. Pending JNI exceptions and borrowed Parcel ownership stay with
the caller.

Native ARM64 GLES imports, dynamic symbols and procedure lookups adapt spilled
arguments from Android's eight-byte stack slots to Darwin's natural alignment.
The complete GLES headers supply the function types. Signatures with
matching layouts keep the original provider pointer, and AOSP JNI and ARM32's
typed native bridge call the Darwin entry points directly.

### EGL synchronization and errors

Metal synchronization needs an allocated event and command buffer before a
queued sync is created. Allocation failure returns `EGL_BAD_ALLOC` instead of
publishing a native fence backed only by a queue serial, and ART passes that
allocation error through its host-call boundary without turning it into a
context mismatch. Imported shared events that do not enqueue a signal keep their
original synchronization behavior.

The ART native-fence producer observes its actual Metal command buffer. A failed
completion finishes the original sync-file writer with an I/O error, and
successful completion is handled by the shared-event listener. Guest
native-fence client waits, server waits and status queries read the original
sync-file status once the descriptor is ready. A negative status returns an EGL
error, while successful fences and timeouts keep their existing results.
Descriptor duplication, waits and closing keep the original ABI.

The C EGL presenter reports typed EGL errors without creating Java exceptions.
Allocation failure returns `EGL_BAD_ALLOC`, and a failed GPU completion returns
`EGL_CONTEXT_LOST`. JNI presenter entry points keep their Java exception
contract and preserve an exception already pending on the calling thread.
Surface teardown releases work that has finished and keeps dependencies while
GPU work is still in flight.

A failed context binding restores the previous context and surfaces before
returning the original EGL error. If a device failure also prevents that
restoration, the context is marked lost before later GL calls.

## OpenSL ES and OpenMAX AL

OpenSL ES and OpenMAX AL interface queries use the complete original IID and
type catalog, including local-library results and application-owned copies of
UUIDs. Their tagged source and sink locators and formats use the original NDK
record layouts and compiler-evaluated discriminators, including Android
extensions.

## Memory reporting

JVM `Process.getTotalMemory` and `getFreeMemory` use the same native memory
accounting as `ActivityManager` and `Bridge.memoryInfo`. The configured
`KL_MEM_TOTAL_MB` guest budget is capped by physical memory, and available bytes
follow the live process headroom. `/proc/meminfo` reports that budget in kB
together with the existing process-footprint accounting. These APIs do not
expose a separate host-wide RAM or free-page estimate.

ART keeps the original Bionic `sysinfo`, `sysconf` and file APIs, while AIM's
existing memory accounting applies the same 6144 MiB default, configured MiB
conversion and physical-memory cap. ART available memory follows the actual
process footprint and native headroom, limited by that budget. Linux kernel and
cache categories without guest accounting report zero, and actual macOS
memory-pressure notifications keep their original watcher.

## Synchronization and diagnostics

Private futexes keep exact waiter selection and requeue ownership, while Darwin
kernel address waits handle sleeping and waking outside native metadata locks.
Short critical sections on that metadata defer asynchronous signals through the
original POSIX mask. Synchronous faults keep their execution-domain handler, and
waits restore the calling thread's mask.

Native diagnostics in the same process can copy a size-limited snapshot of
private futex waiters from that queue. The snapshot keeps the original address,
operation, expected value, timeout and pthread owner, including address changes
from requeue. It never reads the guest futex word or waits for the metadata
lock.

ART's optional `MACLAUNCHER_ART_ABORT_CONTEXT=1` diagnostic records up to 16
abort requests that a process directs at itself, at their original syscall
boundary, including a limited frame-pointer walk. Signal delivery is unchanged,
and the record distinguishes an abort request from a fatal signal. Output goes
through the existing diagnostics channel without allocating, resolving symbols
or waiting for a stream reader.

## Services and permissions

Each descriptor handoff has its own receipt pipe, on which the receiver
acknowledges the transferred descriptors. Once a data socket is adopted, closing
its receipt pipe does not close the independent parent process-authority stream, and data connections keep their own peer-failure
handling.

Native Android permission queries use the profile service's original Binder
interface and the same package grants and explicit device-access policies as
Java clients. Native service UIDs receive only the permissions that their
original platform configuration assigns, and those assignments never grant
application UIDs access to protected devices or services.

Native audio permissions use Android's original permission provider and service
notifications. Package changes and permission updates share the existing
package catalog of the profile services, and the original audio synchronization barrier waits for
pending updates.

## Developer build

A developer build takes supplied executable framework and native artifacts
together with source inputs at their recorded revisions, while installed runtime
packages run without an Android SDK or native compiler. Developer prerequisites are JDK 21, the official SDL3
framework, Vulkan and NDK headers, portable codec libraries, AIDL and aconfig, a
Rust toolchain, Ninja, CMake and a project Python environment.

Default source locations come from `build-options.mjs`, the font and KeyStore
catalogs and the NDK graphics source catalog. Explicit `--<name>-source`
arguments select equivalent source checkouts at the same revisions. The original
Blueprint parser executable, together with its recorded source revision, defines
the declaration syntax and source-group evaluation; the normal build does not evaluate an Android product
graph.

```sh
node src/modules/android/native/build.mjs \
  --klepton src/external/klepton \
  --converter src/external/dex2jvm \
  --framework-source src/external/aosp-framework \
  --native-source src/external/aosp-native \
  --av-source src/external/aosp-av \
  --wilhelm-source src/external/aosp-wilhelm \
  --libcore-source src/external/aosp-xmlpull \
  --art-source src/external/aosp-art \
  --conscrypt-source src/external/aosp-conscrypt \
  --boringssl-source src/external/aosp-boringssl \
  --libbase-source src/external/aosp-libbase \
  --fmt-source src/external/aosp-fmt \
  --jdk /path/to/jdk/Contents/Home \
  --aidl /path/to/aidl \
  --aconfig /path/to/aconfig \
  --rust-toolchain /path/to/rust/toolchain \
  --python /path/to/project/.venv/bin/python \
  --sdl /path/to/SDL3.framework \
  --vulkan-headers /path/to/vulkan/include \
  --ndk-headers /path/to/android/ndk/sysroot/usr/include \
  --sndfile-headers /path/to/libsndfile/include \
  --codecs /path/to/portable/codecs \
  --angle /path/to/recorded/ANGLE/libraries \
  --gles-headers src/external/krkrsdl2/external/krkrz/external/angle \
  --framework /path/to/verified/framework/artifact/directory \
  --blueprint-parser /path/to/android-blueprint-parser \
  --blueprint-parser-sources /path/to/parser-sources.json \
  --output /path/to/new/build
```

`--build-work` selects a persistent compiler workspace. The build snapshots
MacLauncher's inputs before compiling and reuses unchanged Java modules, native
objects and supplier outputs. Conversion caches depend only on DEX inputs,
converter options and the framework hierarchy, so unrelated native source
changes do not invalidate them. Missing upstream Git objects and Cargo
dependencies can be downloaded during developer setup, and cached inputs allow
offline compilation.

### Incremental reuse

KeyMint source exports and generated AIDL files keep unchanged content, Clang
dependency files let C++ objects be reused, and static archives update only when
their object inputs change. Generated JNI and host callback declarations keep
their native compiler timestamps when Java implementations change, and removed
declarations are deleted during publication. Framework transformation reuse
follows the compiled native binding declarations, while host Java
implementations are rebuilt separately. ARM32 packed-relocation and Linux UAPI
declarations use the same content-preserving publication.

Compiler prefix maps remove checkout and toolchain paths from delivered objects.
They cover the source, build and referenced input roots, so changing an
unrelated runtime provider does not invalidate native objects. Clang dependency
files track header dependencies.

### Delivered binaries

Native delivery copies keep relocatable install names, have their compiler debug
records stripped and are signed independently of cached development products. A
changed signed binary replaces the destination atomically with a new file, so
running mappings and open readers keep the previous binary. When the bytes and
executable permissions are identical, the destination and its modification time
are left alone. Published permissions never include the set-user-ID,
set-group-ID or sticky bits. Staging applies the same debug-record removal and
signing to verified ANGLE copies, while the supplied Android framework library
is kept byte for byte. Licence texts live in the flat `licenses/` directory, with component
associations in `licenses/components.txt`. Source records keep the exact upstream
revisions. A build emits a complete list of its outputs, its source revision and
its runtime profile, and staging uses them to install the build atomically.

## ANGLE

ANGLE's native build keeps upstream's embedded internal Metal shader library.
Its original GN actions call Apple's `metal` and `metallib` tools through
`xcrun`, and both the separate Metal toolchain and the standard `TOOLCHAINS`
selection are supported. The resulting shader bytes live inside
`libGLESv2.dylib`, so application startup needs no separate shader-file search
or compiler. The driver version includes the exact ANGLE revision and its
generated program binary version, so application-owned shader caches can use
the original GL version and renderer strings to tell incompatible binary formats
apart after updates.

The Metal backend keeps the upstream framebuffer flip state from ANGLE
`37c976ffba9d2940add977cad9945b23de14c150`. `GL_MESA_framebuffer_flip_y`
combines the requested framebuffer origin with the native backbuffer origin, and
viewport, scissor, winding, clears and blits follow that state. AIM uses this
operation to produce Android window buffers with the top row first. CPU buffers
and framebuffers without an explicit flip keep their ordinary row order.

The combined ANGLE provider exports `kljAngleVkImageOwnership(device, image,
transfer)` for native Android buffer storage:

| `transfer` | Effect |
| --- | --- |
| `1` | Registers shared storage |
| `2` | Registers a converted Metal texture |
| `0` | Cancels an unconsumed transfer |

After a pending transfer is registered, the next `EGL_ANGLE_vulkan_image` import
adopts that `VkImage`. Ordinary external image imports keep their borrowed
lifetime. Adopted images are released through ANGLE's final GL reference and
Vulkan garbage queue. Shared VM aliases follow the Metal buffer's deallocator,
so an Android mapping can close while a GL object still holds the storage.
ANGLE's original foreign-image submission tracker publishes shared image writes
through an exact Vulkan host-access barrier in the same command batch and
resource serial as the GL work. Normal EGL and native fences cover these writes,
including writes to imported textures rendered outside window presentation.

Planar video images keep the same image lease and CoreImage conversion in both
backends. Vulkan uses that lease's timeline semaphores and their exported Metal
shared events to order earlier GL reads, the conversion and later GL reads on
the original ANGLE queue. The lease completes its pending conversion and queue
wait before releasing source storage or synchronization objects, and the
converted RGB texture follows the last GL reference independently.

## Distribution

[MacLauncher Builds](https://github.com/procrastinine/maclauncher-builds)
packages the completed build outputs with their source revisions and notices as
versioned packages, which MacLauncher installs.

## Process identity leases

Released process and thread IDs are reused before higher slots are allocated.
Allocation starts at the lowest slot, so a new process reuses released kernel
leases after a crash. Live and unreaped child identities retain their existing
kernel locks; stable lease files remain in place.
