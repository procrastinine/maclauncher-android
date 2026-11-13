# Android source and runtime reference

MacLauncher Android is the source of an Android runtime for macOS on Apple
Silicon, which keeps the complete Android framework APIs and adds native
graphics, audio, input and platform adapters. This reference covers the source
tree, its build inputs and producers (the build scripts that turn those inputs
into runtime packages), and selected runtime behavior. Paths and commands are
relative to the MacLauncher Android checkout.

This repository's source tree contains the JVM host, the ART adaptations, native
implementations, MacLauncher's patches to upstream projects, the exact upstream
commits. The launcher UI, game import and the launcher's account
and storage handling stay in MacLauncher.

MacLauncher includes this project as a Git submodule at an exact commit.
MacLauncher's producer paths are source-tree aliases into this repository, and
the runtime implementation lives here. The MacLauncher application includes the
small APK metadata and integration helpers that the launcher uses, while the
large common and ART runtimes are separate packages published by MacLauncher
Builds.

## Upstream sources and licenses

`npm run check` validates source import paths and the exact upstream Git
commits. `upstreams.json` records the source URLs and commits of the Git modules. The HWUI
component's original sources are recorded with their commits and checked Git objects in
`src/modules/android/native/hwui/sources.json`, from which its producer acquires them.
The Metal `GL_MESA_framebuffer_flip_y` framebuffer-state handling in the ANGLE adaptation
follows ANGLE commit 37c976ffba9d2940add977cad9945b23de14c150
(`src/modules/android/native/angle-flip-source.json`). The JVM portability call adapters
follow Robolectric commit 3cfa3a4b8e2910ae06ce0d12e6f7bd927f8a5d2e
(`src/modules/android/native/sources/robolectric-portability/source.json`).
Upstream checkouts are read-only inputs, and adaptations are applied to
staged copies of the sources that belong to this repository's build. The source
check can verify source Gitlinks and metadata without initializing checkouts;
initialized checkouts are also checked for their exact HEAD commit and for
changes to tracked files. The
source check downloads nothing.

The root LICENSE covers code written for MacLauncher. Upstream source fragments
and build inputs keep their original licenses, which are recorded in source
headers and packaged texts. The repository does not
relicense upstream components.

## Common runtime builds

The common runtime's native producer takes its build inputs as explicit paths:

```sh
node tools/build.mjs --inputs /absolute/path/to/build-inputs.json
```

The JSON object uses the native producer's option names without leading dashes.
It supplies the source checkouts at their recorded revisions, the JDK, platform tools, portable
dependency artifacts and the output and cache directories. The explicit build
command downloads the build-time Maven tools and verifies them against their
recorded checksums. Verified Maven tools are cached in the selected build
workspace and copied into its fixed source snapshot, and source files stay
unchanged during a build. Generated libraries, archives, Android images and
caches are not committed to Git.

### Framework inputs

One build input, the `framework` binary input directory, contains
`android-all-16-robolectric-13921718.jar` and `SettingsProvider.apk`. Their
exact names, checksums and origins are recorded in
`native/framework-binary.json` and `native/platform-packages-binary.json` under
`src/modules/android/`. The original provider APK is extracted unchanged from
`system/priv-app/SettingsProvider/SettingsProvider.apk` in the Android system
image that the latter catalog identifies. The build uses this separately
supplied APK, so a Google-enabled system image is not needed to build or run
the runtime. The producer copies the component
into `platform/`, and the platform component collection contains no complete
image or Google package. The APK is a binary build input and is not committed to
this repository.

Framework staging keeps the original hyphenation data from the selected
framework artifact, and text layout and widgets use the same language patterns.

## ART builds

### Build inputs

The standalone ART producer builds an ART supplier, a directory of ART runtime
components with a manifest from which the ART package is later assembled:

```sh
node tools/build-art.mjs --inputs /absolute/path/to/art-inputs.json
```

The input object supplies the `buildArt` options, including the `host`, `java`,
`inputs` and `bootClasspath` producer groups. `sources.source`,
`sources.klepton` and `sources.glesHeaders` name the original upstream checkouts.
Without `sourceArchive`, `sources.aosp` supplies the original AOSP tree used by
AIM's `patches/art-android/sources.lock`, and the exporter checks the original
ART, bionic, libcore and dependency subsets against the recorded content hashes.
The Binder source checkout is also included. `sourceCheckouts` can map
registered upstream identifiers to other checkout paths or add source inputs
used by separately supplied components. Source export downloads nothing, and
source inputs stay separate from compiler outputs.

If `inputs.art` is absent, `artBuild.sdk` supplies the Android SDK with the NDK
version that AIM specifies, and `artBuild.fetched` supplies AIM's original fetched-input
directory, which defaults to the parent of `sources.aosp`. The producer then
runs AIM's original ART build graph, including its source locks, patches,
compiler commands and stripping. An explicit `inputs.art` uses a separately
produced ART root instead. Further options:

| Option | Effect |
| --- | --- |
| `buildWork` | Reusable absolute workspace |
| `artBuild.work` | Separate ART compiler target directory |
| `artBuild.jobs` | Limit on compiler parallelism (default 4) |
| `artBuild.jdk` | JDK home |
| `artBuild.python` | Directory of the Python executable used by the original graph |

The result includes the supplier `root` and `sourceArchive`. The default archive
contains this repository's producer sources, the selected upstream source inputs
and license notices, without test sources or generated build files. An explicit
absolute `sourceArchive` keeps an existing complete corresponding-source archive
unchanged. Separately supplied HWUI and native bridge components need their
corresponding original inputs or a complete explicit archive.

The ordinary producer reuses its `hwui.sources`, Cargo cache and prepared or
generated component source records. Bridge sources reuse the declared Bionic and ART
checkouts and the original import-header input. Top-level source options can
supply original inputs for retained components.

`hwuiSources` contains the public HWUI `sources` mapping, the active Cargo
`registry` records from its `sourceRecords/SOURCES.json` output, and an absolute `registryCache` directory. The exporter
verifies the source catalog and the Cargo.lock checksums, then includes the
original source files that the build used, notices and registry packages.

`nativeBridgeSources.ndkHeaders` selects the original ARM32 NDK headers recorded
for the build.
`nativeBridgeSources.arm32Sources` selects the ARM32 source-record directory,
which records the Bionic units the component uses; the ordinary producer reuses
`nativeBridge.arm32.sources`. `sourceCheckouts` supplies `libffi`, `dynarmic`,
`aosp-bionic` and `aosp-art`. The source archive keeps the selected original
headers, their case-sensitive variants and a record that identifies
the compiler-runtime artifact. Android system images, complete SDK and NDK installations and
compiler products are supplied separately.

### Package assembly

A separate producer, `src/modules/android/art/package.mjs`, assembles the ART
package. Its inputs include the completed supplier, the common runtime, the verified checksums of
the original image, the extractor and the corresponding source archive.
`commonDeclaration` supplies the common runtime identity and must match
the SHA-256 of the common producer's manifest. The caller supplies the shared package
inventory, Mach-O inspection and verification services; the producer reads no
launcher catalog or selected version.

The optional `hwuiLibrary` supplier input delivers a complete
Android graphics library next to the original image.
`assembleArtSupplier` accepts `includeImage: false` with verified
`originalImage` checksums to package an adapter from existing components. The
image is still obtained separately and is neither copied nor read during that
assembly. The shared collector stores available texts in `licenses/`, with
component associations in `licenses/components.txt`. Packaging copies these
files with their component directories as ordinary inventory members.

`prepareAimSource` accepts an explicit `glesHeaders` checkout alongside its AIM
and Klepton inputs, and verifies the header bytes and the original Git revision
before use. Without that input, standalone builds use the registered nested
GLES-header source checkout.

### Runner

ART also has a runner producer, `src/modules/android/art/build-runner.mjs`,
which takes a prepared AIM source tree and statically links its ARM64 execution and native
continuation components into `host/linux-run`. A supplied `buildPrecise` result
reuses existing CPU archives; otherwise the producer builds Unicorn from source
at its recorded revision. `buildArt` accepts this component through its optional `runner` input
and collects available top-level licence texts through the shared collector.
Missing or unreadable licence sources emit a warning.

ART Java compilation uses the original Android core declaration image. The
complete `android.system` and `dalvik.system` package declarations from the
framework archive patch `java.base` during compilation, preserving access to
hidden sibling classes such as `SocketTagger`. These declarations remain in
the compiler workspace.

## Distribution

MacLauncher Builds publishes binary-only runtime release archives, and its
tagged source tree contains the build recipes, MacLauncher's patches and this
repository at the commit that was built.

## Runtime behavior

The [Android runtime architecture](runtimes/ANDROID_ARCHITECTURE.md) and
[Android native code](runtimes/ANDROID_NATIVE_CODE.md) pages describe the
runtime as a whole. This section adds details about individual services and
native interfaces.

### Framework and services

APK metadata inspection supports feature components with the base application's
defaults. Non-isolated splits share the application loader, and isolated
feature splits keep Android's original dependency loaders and split-specific
component contexts.

The JVM ImageReader binding keeps the original ImageReader, Image and plane
objects over the shared Surface and HardwareBuffer handling. App-controlled
producers replace queued frames without waiting for their own consumer, and
acquired images and returned hardware buffers hold independent references.
Public, HAL and dataspace formats use the original Android device mappings.
Buffer planes keep their producer dimensions, format, row stride and pixel
stride, and PRIVATE buffers expose opaque storage without CPU planes. The
[architecture page](runtimes/ANDROID_ARCHITECTURE.md#surfaces-buffers-and-video)
lists the native storage formats and allocation limits.

The Binder JNI library registers CursorWindow parcel methods after Parcel
metadata initialization. Inline and mapped windows keep their original Android
serialization and descriptor ownership.

Application-operation mode changes use the existing permission-decision store
and Android's management permission checks. Watchers keep their registered
operation, flags and Binder lifetime, and effective foreground modes follow
Activity state. Shutting down the PendingIntent queue cancels the original
tokens before their alarm service is unpublished.

AAudio output progress follows each stream's committed consumed frames in that
stream's sample-rate domain. Atomic snapshots keep progress getters independent
of the mixer's paced writer lock. Device cursor resets keep the consumed
history, and streams keep separate counters and their final values after
release.

### Native code

The JVM and ART builds share native instruction analysis. Intact
stack-protected continuations keep the original Android thread-pointer and
register behavior without changing opaque prefixes or explicit data.

The ART loader keeps the initial argument table and writable stack below the
shared argument-string pages, so process command-line records continue to
reflect changes to the original argument strings.

Named ELF library handles resolve symbols in the owning image and, in
breadth-first order, its original `DT_NEEDED` dependencies and theirs. Platform dependencies
use the existing Android ABI adapters. Unrelated loaded images stay outside that
handle's scope, and missing symbols give the normal `dlerror` result.

Descriptor-less virtual stdio streams reach the original close callback without
trying to release a nonexistent file descriptor.

The lease that holds a guest process's Android process ID also records Linux
process dumpability. Normal executables
start dumpable, fork inherits the setting, and an ordinary exec resets it.
Disabling dumpability restricts inspection by other processes, while a process
can still access itself. Inspection is also subject to the host sandbox and
task-port restrictions, and these controls never grant host debugger access.

Native POSIX memory advice preserves errno and modified private bytes. The ARM64
`sendfile` and `sendfile64` interfaces keep Linux argument order, explicit or
current input offsets, partial-transfer counts and nonblocking errors for
regular-file input with file, socket or pipe output. Linux hole punching
deallocates whole filesystem blocks and zeroes partial edges while keeping the
file size and current offset, and native filesystem errors are still reported as
errors.

Internet stream sockets keep Linux connection retry results across interrupted
and nonblocking connects. A socket that the descriptor layer
allocates carries an allocation receipt, a trusted record of its inode that is
checked when the socket is adopted. Inherited sockets without such a receipt are
passed as their original descriptor.

The JVM native boundary uses a 65,536-slot Bionic semaphore table. Slot zero
represents an uninitialized semaphore; released slots are reused.
