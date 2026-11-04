# Android source and runtime reference

Paths and commands in this reference are relative to the MacLauncher Android checkout.

Android runtime source for macOS on Apple Silicon. The runtime retains complete
Android framework APIs with native graphics, audio, input and platform adapters.

The source tree contains the JVM host, ART adaptations, native implementations,
owned upstream patches, source pins and notices. Launcher UI, game import and
launcher account/storage orchestration remain in MacLauncher.

MacLauncher consumes this project as a pinned source submodule. Its existing
producer paths are source-only aliases into this repository; the runtime
implementation lives here. The MacLauncher application includes the small APK
metadata and integration helpers used by the launcher. Bulk common and ART
runtimes are installed as managed packages from MacLauncher Builds. Running
games does not require Git, source checkouts or compilation.

## Common runtime builds

Build inputs are explicit paths supplied to the existing native producer:

```sh
node tools/build.mjs --inputs /absolute/path/to/build-inputs.json
```

The JSON object uses the native producer's option names without leading dashes.
It supplies the pinned source checkouts, JDK, platform tools, portable dependency
artifacts and output/cache directories. Build-time Maven tools are acquired by
the explicit build command and verified against their recorded checksums.
Generated libraries, archives, Android images and caches are excluded from Git.
Verified Maven tools are cached under the selected build workspace and copied
into its fixed source snapshot; source files remain unchanged during a build.

`npm run check` validates source import paths and exact upstream Git pins.
`upstreams.json` records source URLs and commits. Upstream checkouts
are read-only inputs; adaptations are applied to owned build projections.

The root LICENSE covers MacLauncher-owned code. Upstream source fragments and
build inputs retain their original licenses in source headers and packaged texts. The repository does not relicense upstream
components. Source Gitlinks and metadata can be checked without initializing
checkouts; initialized checkouts are also checked for exact HEAD identity and
tracked changes. The source check performs no downloads.

## Distribution

MacLauncher Builds publishes binary-only runtime release archives. Its tagged
source tree contains the build recipes, owned patches and pinned Android source.
Runtime installation and updates are explicit launcher actions; this source
project performs no background update checks.

## Framework and native services

Native instruction analysis is shared by the JVM and ART producers. Intact
stack-protected continuations retain the original Android thread-pointer and
register behavior without changing opaque prefixes or explicit data. APK
metadata inspection supports feature components with base application defaults.
Non-isolated splits share the application loader. Isolated feature splits retain
Android's original dependency loaders and split-specific component contexts.

The `framework` binary input directory contains
`android-all-16-robolectric-13921718.jar` and `SettingsProvider.apk`. Exact names,
checksums and provenance are recorded in `native/framework-binary.json` and
`native/platform-packages-binary.json` under `src/modules/android/`. The original
provider APK is extracted unchanged from
`system/priv-app/SettingsProvider/SettingsProvider.apk` in the Android system image
identified by the latter catalog. The build consumes the separately supplied
APK; a Google-enabled system image is not a build or runtime prerequisite. The
producer stages this component under `platform/`;
no complete image or Google package is included in the platform component
collection. The APK is a binary build input and is not committed to this source
repository.

Framework staging retains the original hyphenation data supplied by the selected
framework artifact. Text layout and widgets consume the same language patterns.

The JVM ImageReader binding keeps the original ImageReader, Image and plane
objects over the shared Surface and HardwareBuffer owners. App-controlled
producers replace queued frames without waiting for their own consumer; acquired
images and returned hardware buffers retain independent references. Public,
HAL and dataspace formats use the original Android device mappings. Buffer
planes retain their producer dimensions, format, row stride and pixel stride;
PRIVATE buffers expose opaque storage without CPU planes. The
[architecture reference](runtimes/ANDROID_ARCHITECTURE.md) records the native
storage formats and allocation limits.

CursorWindow parcel methods are registered by the Binder JNI owner after Parcel
metadata initialization. Inline and mapped windows retain their original Android
serialization and descriptor ownership.

Application-operation mode changes use the existing permission-decision store
and Android management permission checks. Watchers retain their registered
operation, flags and Binder lifetime; effective foreground modes follow activity
state. PendingIntent queue retirement cancels original tokens before their alarm
service is unpublished.

AAudio output progress follows each stream's committed consumed frames in that
stream's sample-rate domain. Atomic snapshots keep progress getters independent
of the mixer's paced writer lock. Device cursor resets preserve consumed history;
streams retain separate counters and final released values.

Descriptor-less virtual stdio streams reach the original close callback without
attempting to release a nonexistent file descriptor.

Linux process dumpability belongs to the existing guest process lease. Normal
executables start dumpable, fork inherits the setting, and ordinary exec resets
it. Disabling dumpability restricts peer inspection; self-access remains
available. Inspection also remains subject to the host sandbox and task-port
restrictions. These controls do not grant host debugger access.

Native POSIX memory advice preserves errno and modified private bytes. The ARM64
`sendfile` and `sendfile64` interfaces retain Linux argument order, explicit or
current input offsets, partial-transfer counts and nonblocking errors for
regular-file input with file, socket or pipe output. Linux hole punching deallocates whole
filesystem blocks and zeroes partial edges while retaining file size and current
offset; native filesystem errors remain errors.

## ART package assembly

The ART package producer is `src/modules/android/art/package.mjs`. Its inputs
include the completed supplier, common runtime, verified original image identity,
extractor and corresponding source archive. `commonDeclaration` supplies the
common runtime identity and must match that producer manifest SHA-256.
The caller supplies shared package inventory, Mach-O inspection and verification
services; the producer does not read a launcher catalog or selected version.
The optional `hwuiLibrary` supplier input delivers a complete
Android graphics library beside the original image.
`assembleArtSupplier` accepts `includeImage: false` with a verified
`originalImage` identity for packaging an adapter from existing components.
The image remains separately acquired; it is neither copied nor read during
that assembly. The shared collector stores available texts in `licenses/`, with
component associations in `licenses/components.txt`. Packaging copies these
files with their component directories as ordinary inventory members.

`prepareAimSource` accepts an explicit `glesHeaders` checkout alongside its AIM
and Klepton inputs. Header bytes and the original Git revision are verified
before use. Without that input, standalone builds use the registered nested
GLES-header source checkout.

## ART build inputs

ART suppliers build through the standalone producer:

```sh
node tools/build-art.mjs --inputs /absolute/path/to/art-inputs.json
```

The input object supplies the existing `buildArt` options, including `host`,
`java`, `inputs` and `bootClasspath` producer groups. Its `sources.source`,
`sources.klepton` and `sources.glesHeaders` name the original pinned checkouts.
Without `sourceArchive`, `sources.aosp` supplies the original AOSP tree used by
AIM's `patches/art-android/sources.lock`. The exporter checks those original
ART, bionic, libcore and dependency subsets against the recorded content hashes.
The pinned Binder source checkout is also included; `sourceCheckouts` can map
registered upstream identifiers to alternate source checkout paths or add
source inputs used by separately supplied components. Source export performs no
downloads. Source inputs remain separate from compiler outputs.

If `inputs.art` is absent, `artBuild.sdk` supplies the Android SDK with AIM's
pinned NDK, and `artBuild.fetched` supplies AIM's original fetched-input directory
(defaulting to the parent of `sources.aosp`). The ordinary producer runs the
original AIM ART build graph, including its source locks, patches, compiler
commands and stripping. `buildWork` selects a reusable absolute workspace;
`artBuild.work` can select its ART compiler target directory separately.
`artBuild.jobs` bounds compiler parallelism (default 4).
`artBuild.jdk` selects a JDK home and `artBuild.python` selects the Python
executable directory used by the original graph. An
explicit `inputs.art` retains a separately produced ART root.

The result includes the supplier `root` and `sourceArchive`. The default archive
contains the owned producer sources, the selected upstream source inputs and
license notices. Test sources and generated build files are omitted. An explicit
absolute `sourceArchive` preserves an existing complete corresponding-source
archive unchanged. Separately supplied HWUI and native bridge components require
their corresponding original inputs or a complete explicit archive.

The ordinary producer reuses its `hwui.sources`, Cargo cache and prepared or
generated component source records. Bridge sources reuse its declared Bionic/ART
checkouts and original import-header input. Top-level source options can supply
original inputs for retained components.

`hwuiSources` contains the public HWUI `sources` mapping, the active Cargo
`registry` records from its `sourceRecords/SOURCES.json` output,
and an absolute `registryCache` directory. The exporter verifies the pinned
source catalog and Cargo.lock checksums, then includes consumed original source
projections, notices and registry packages. `nativeBridgeSources.ndkHeaders`
selects the pinned original ARM32 NDK headers. `nativeBridgeSources.arm32Sources`
selects the ARM32 source-record directory that records its consumed Bionic
units; the ordinary producer reuses `nativeBridge.arm32.sources`. `sourceCheckouts` supplies
`libffi`, `dynarmic`, `aosp-bionic` and `aosp-art`. The source archive retains the
selected original headers, their case-sensitive variants and
compiler-runtime artifact identity. Android system images, complete SDK/NDK
installations and compiler products are supplied separately.

## ART runner

The ART runner producer, `src/modules/android/art/build-runner.mjs`, accepts a
prepared AIM source tree and statically links its ARM64 execution and native
continuation components into `host/linux-run`. A supplied `buildPrecise` result
reuses existing CPU archives; otherwise the producer builds the pinned Unicorn
source. `buildArt` accepts this component through its optional `runner` input
and collects available top-level licence texts through the shared collector.
Missing or unreadable licence sources emit a warning.

## Native runtime behavior

The ART loader keeps the initial argument table and writable stack below the
shared argument-string pages. Process command-line records continue to reflect
changes to the original argument strings.

Named ELF library handles resolve symbols in the owning image and its original
`DT_NEEDED` dependency closure, in breadth-first order. Platform dependencies use
the existing Android ABI adapters. Unrelated loaded images remain outside that
handle scope; missing symbols retain the normal `dlerror` result.

Internet stream sockets retain Linux connection retry results across interrupted
and nonblocking connects. Inherited sockets without a socket inode receipt
travel as their original descriptor; allocated sockets retain receipt validation.
