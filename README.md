# MacLauncher Android

Android runtime source for macOS on Apple Silicon. The runtime retains complete
Android framework APIs with native graphics, audio, input and platform adapters.

The source tree contains the JVM host, ART adaptations, native implementations,
owned upstream patches, source pins and notices. Launcher UI, game import and
launcher account/storage orchestration remain in MacLauncher.

MacLauncher consumes this project as a pinned source submodule. Its existing
producer paths are source-only aliases into this repository; implementation
changes belong here. Application packages contain only the small metadata
library needed by the launcher and the selected runtime binaries. Running games
does not require Git, source checkouts or compilation.

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
`upstreams.json` records source URLs, commits and license notice paths. Upstream checkouts
are read-only inputs; adaptations are applied to owned build projections.

The root LICENSE covers MacLauncher-owned code. Upstream source fragments and
build inputs retain their original licenses, recorded in source headers,
upstreams.json and emitted notices. The repository does not relicense upstream
components. Source Gitlinks and metadata can be checked without initializing
checkouts; initialized checkouts are also checked for exact HEAD identity and
tracked changes. The source check performs no downloads.

maclauncher-builds packages runtime releases and their corresponding sources. Runtime installation and updates are explicit launcher
actions; this source project performs no background update checks.

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
producer stages this Apache-licensed component and its notice under `platform/`;
no complete image or Google package is included in the platform component
collection. The APK is a binary build input and is not committed to this source
repository.

The ART package producer is `src/modules/android/art/package.mjs`. Its inputs
include the completed supplier, common runtime, verified original image identity,
extractor and corresponding source archive. `commonDeclaration` supplies the
common runtime notice records and must match that producer manifest SHA-256.
The caller supplies shared package inventory, Mach-O inspection and verification
services; the producer does not read a launcher catalog or selected version.
