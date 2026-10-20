# Android native host

The [architecture reference](../../../../docs/runtimes/ANDROID_ARCHITECTURE.md)
describes execution, platform boundaries, resource ownership and host policy.
The included backend combines Java 21, the complete Android 16 Java framework,
dex2jvm and pinned Klepton native code. ARM32 libraries use Dynarmic and the same
platform services as ARM64.

## Source layout

| Sources | Responsibility |
| --- | --- |
| `java/maclauncher/android` | Application entry, original framework bootstrap, package metadata and process authority |
| `framework`, `converter`, `portability` | Framework assembly and exact JVM/native dependency adaptations |
| `nativebinding.c`, `loader-*.c`, `nativeabi.*`, `ndk-jni` | Native loading, original JNI declarations and Android/Darwin calling conventions |
| `arm32` | ARM/Thumb execution, ELF32 records, AAPCS32 and JNI width conversion |
| `binder-rpc`, `activity-thread`, `keystore` | Shared Binder transport, application lifecycle and profile service implementations |
| `native-*.mm`, `window*`, `presentation.mm`, `clock` | AppKit, input, display, surfaces and frame scheduling |
| `gles-guest-abi.*` | Original GLES declaration types and AAPCS64-to-Darwin stack adapters |
| `media-foundation`, `audio*`, `video*` | Original media-format algorithms and native playback/device operations |
| `libcore`, `conscrypt`, `process-abi`, `native-clock` | Original portable libraries and precise Darwin ABI adaptation |
| `file-events`, `pollable-fds` | Shared file-descriptor lifetime, polling, counters, timers and directory state |
| `fonts`, `system-config.mjs` | Pinned Android fonts and platform configuration |
| `upstream` | Source export, original Blueprint parsing, compiler inputs and incremental reuse utilities |

Each source catalog records original repository and revision identities. Original
checkouts remain unchanged; owned adaptations are applied to exported compiler
inputs. The complete supplied framework retains its class and method surface.
Both VM entries publish Android's original process font map before application
UI construction; an existing map retains its Typeface objects and backing memory.
Presentation cleanup uses the shared task owner across Activity navigation and
configuration relaunches. It retires the current Activity stack on its owner thread
before releasing view roots, display callbacks and the native output.
SurfaceControl transaction completion follows the existing output's consumed
frame. Native output uses the common presenter's observed display retirement;
dropped or merely encoded images do not report completion. TransactionStats
retains the original latch timestamp API and an empty present fence when no
Android fence descriptor is exported by that output.
The original Apache HTTP and StatsD modules are compiled from their complete
published source groups against that framework.
Publisher JNI families are enumerated from the supplied native library and kept
whenever their final framework classes retain native declarations. The build does
not select registrations by package or class-name conventions.

Android native libraries receive a JNI environment facade while real Java objects
remain owned by the JVM. Android and Darwin argument layouts are selected from
the calling code's registered image. Per-thread JNI state and attachments retire
at the corresponding thread exit boundary, including host-created callbacks.
Private futexes retain exact waiter selection and requeue ownership while Darwin
kernel address waits handle sleeping and waking outside native metadata locks.
Short metadata critical sections defer asynchronous signals through the original
POSIX mask; synchronous faults retain their execution-domain handler, and waits
restore the calling thread's mask.
Own-process native diagnostics can copy a bounded snapshot of private futex
waiters from that same queue. The snapshot retains the original address,
operation, expected value, timeout and pthread owner, including address changes
from requeue. It never reads the guest futex word or waits for the metadata lock.
Native library state and class references retain their library's load lifetime.
Native ABI metadata lookup retains the calling library's class-loader namespace.
The configured application ABI selects ELF dependencies and system library paths
before native constructors run. ARM32 and ARM64 imports retain the same platform
service providers and per-library class-loader ownership.
Large ARM64 ELF images reserve instruction veneers at both ends of their mapping
and use original gaps between load segments for sites outside both branch
windows. Every entry and return branch is checked before instructions change.
Original segment bytes retain their addresses; packed veneer pages contain no
writable original segment, and used pools finish read-only and executable.
Layouts without sufficient reachable storage report a loader error.
ARM64 ELF TLS supports `PT_TLS`, `DTPMOD64`, `DTPREL64`, `TPREL64` and `TLSDESC`
while preserving the real Android thread pointer and original Bionic slots.
Initial-exec and dynamic references share the same per-thread address. A private
demand-zero reserve provides one MiB of fixed TLS layout with alignment up to
64 KiB; module offsets remain reserved after unloading. New modules initialize
existing physical threads before native entry points become available. Modules
beyond that layout retain dynamic TLS; an initial-exec reference to one reports
a loader error rather than exposing an unrelated address.
The framework binary input directory also contains the recorded original
SettingsProvider APK. The producer verifies its checksum and manifest, includes
the unchanged APK and resources under `platform/packages/`, and emits the
original package metadata in `platform/packages.json`. Its Apache license notice
and separate source/image identities accompany the component. The image's other
packages are not part of this platform component collection.
Native ARM64 GLES imports, dynamic symbols and procedure lookups adapt spilled
arguments from Android's eight-byte stack slots to Darwin's natural alignment.
The complete pinned GLES headers supply the function types. Signatures with
matching layouts retain the original provider pointer; AOSP JNI and ARM32's
typed native bridge use Darwin entry points directly.
OpenSL ES and OpenMAX AL interface queries use the complete original IID/type
catalog, including local-library results and application-owned copies of UUIDs.
Their tagged source and sink locators and formats use the original NDK record
layouts and compiler-evaluated discriminators, including Android extensions.
Descriptor-transfer receipts belong to their individual handoffs. Once a data
socket is adopted, a retired receipt pipe does not retire the independent parent
authority stream; data connections retain their own peer-failure handling.
Native Android permission queries use the profile service's original Binder
interface and the same package grants and explicit device-access policies as Java
clients. Native service UIDs receive only their original platform configuration's
assigned permissions; those assignments do not grant application UIDs access to
protected devices or services.
Native audio permissions use Android's original permission provider and service
notifications. Package changes and permission updates share the existing profile
catalog; the original audio synchronization barrier waits for pending updates.

Package labels and icons use the original parser and resource renderer in a
headless metadata process. Its batch and idle lifetime is separate from game
activities. Cache keys retain package content, locale and metadata format. Game
processes use a private application bundle with the selected JDK, package name
and rendered icon; headless profile services use accessory application behavior.

## Developer build

The build accepts supplied executable framework and native artifacts alongside
pinned source inputs. End users run the included backend without an Android SDK
or native compiler. Developer prerequisites include a JDK 21, the official SDL3
framework, Vulkan and NDK headers, portable codec libraries, AIDL and aconfig,
a Rust toolchain, Ninja, CMake and a project Python environment.

Default source locations come from `build-options.mjs`, the font and KeyStore
catalogs, and the NDK graphics source catalog. Explicit `--<name>-source` arguments
select equivalent pinned source stores. The original Blueprint parser executable
and its source identity describe declaration syntax and source-group evaluation;
the normal build does not evaluate an Android product graph.

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
  --codec-notices /path/to/portable/codec/licenses \
  --angle /path/to/recorded/ANGLE/libraries \
  --gles-headers src/external/krkrsdl2/external/krkrz/external/angle \
  --framework /path/to/verified/framework/artifact/directory \
  --blueprint-parser /path/to/android-blueprint-parser \
  --blueprint-parser-sources /path/to/parser-sources.json \
  --output /path/to/new/build
```

`--build-work` selects a persistent compiler workspace. The build snapshots owned
inputs before compilation and reuses unchanged Java modules, native objects and
supplier outputs. Conversion caches depend on DEX inputs, converter options and
the framework hierarchy, rather than unrelated native source changes. Missing
upstream Git objects and Cargo dependencies can be acquired during developer
setup; cached inputs permit offline compilation.

KeyMint source exports and generated AIDL files preserve unchanged content; Clang
dependency files reuse C++ objects, and static archives update only when their
object inputs change. Generated JNI and host callback declarations retain their
native compiler timestamps when Java implementations change; removed declarations
are retired during publication. Framework transformation reuse follows the compiled
native binding declarations, while host Java implementations are rebuilt separately.
ARM32 packed-relocation and Linux UAPI declarations use the same content-preserving
publication.

Compiler prefix maps remove checkout and toolchain paths from delivered objects.
They cover the source, build and referenced input roots, so changing an unrelated
runtime provider does not invalidate native objects. Clang dependency files retain
ownership of header dependencies.
Native delivery copies retain relocatable install names, strip compiler debug
records and receive signatures independently from cached development products.
Staging applies the same debug-record removal and signing to verified ANGLE
copies while preserving the supplied Android framework library byte for byte.
Source notices preserve licenses and pinned identities. A build emits its complete
product inventory, source identity and runtime profile for atomic staging.

The native ANGLE build retains upstream's embedded internal Metal shader library.
Its original GN actions use Apple's `metal` and `metallib` tools through `xcrun`;
the separate Metal toolchain and standard `TOOLCHAINS` selection are supported.
The resulting shader bytes reside inside `libGLESv2.dylib`, with no separate
shader-file search or compiler requirement during application startup.
The driver version includes the pinned ANGLE revision and its generated program
binary version. Application-owned shader caches can use the original GL version
and renderer strings to distinguish incompatible binary formats after updates.
The Metal backend retains the upstream framebuffer flip state from ANGLE
`37c976ffba9d2940add977cad9945b23de14c150`. `GL_MESA_framebuffer_flip_y`
combines the requested framebuffer origin with the native backbuffer origin;
viewport, scissor, winding, clears and blits follow that state. AIM uses this
operation when producing top-row-first Android window buffers. CPU buffers and
framebuffers without an explicit flip retain their ordinary row order.

The combined ANGLE provider exports `kljAngleVkImageOwnership(device, image,
transfer)` for native Android buffer storage. Transfer `1` enrolls shared
storage, `2` enrolls a converted Metal texture, and `0` cancels an unconsumed
transfer. Enrolling a pending transfer lets
the next `EGL_ANGLE_vulkan_image` import adopt that `VkImage`; cancelling removes
an unconsumed transfer. Ordinary external image imports keep their borrowed
lifetime. Adopted images retire through ANGLE's final GL reference and Vulkan
garbage queue. Shared VM aliases follow the Metal buffer's deallocator, so an
Android mapping can close while a GL object still holds the storage. ANGLE's original
foreign-image submission tracker publishes shared image writes through an exact
Vulkan host-access barrier in the same command batch and resource serial as GL
work. Normal EGL and native fences cover these writes, including imported
textures rendered outside window presentation.
Planar video images retain the same image lease and CoreImage conversion in
both backends. Vulkan uses that lease's timeline semaphores and their exported
Metal shared events to order previous GL reads, conversion, and subsequent GL
reads on the original ANGLE queue. The lease retires its pending conversion
and queue wait before releasing source storage or synchronization objects;
the converted RGB texture follows the last GL reference independently.

`scripts/stage-android.mjs` installs a completed build into the ordinary Android
runtime declaration. Its `--build`, `--klepton`, `--dex2jvm`, `--codec-sources`
and `--gles-headers` arguments identify that build and the corresponding source
notices. Launcher runtime selection reads this declaration for each launch.
