# MacLauncher Android

Android runtime source for macOS on Apple Silicon. The runtime retains complete
Android framework APIs with native graphics, audio, input and platform adapters.

The source tree contains the JVM host, ART adaptations, native implementations,
owned upstream patches, source pins and notices. Launcher UI, game import and
launcher account/storage orchestration remain in MacLauncher.

Build inputs are explicit paths supplied to the existing native producer:

```sh
node tools/build.mjs --inputs /absolute/path/to/build-inputs.json
```

The JSON object uses the native producer's option names without leading dashes.
It supplies the pinned source checkouts, JDK, platform tools, portable dependency
artifacts and output/cache directories. Build-time Maven tools are acquired by
the explicit build command and verified against their recorded checksums.
Generated libraries, archives, Android images and caches are excluded from Git.

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
