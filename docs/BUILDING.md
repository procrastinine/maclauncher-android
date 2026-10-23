# Building

The source tools require Node.js 24 or later and Git. Native runtime builds
require macOS on Apple Silicon and the compiler, JDK, SDK and dependency inputs
selected by the producer configuration.

## Source checks

```sh
npm run check
node --test tests/android-source-check.test.mjs tests/android-source-owner.test.mjs tests/android-art-package-inputs.test.mjs
node tools/build-art.mjs --check
```

These commands check source imports, upstream revisions, source ownership and
package input requirements without downloading or compiling runtime components.
The GitHub source workflow runs the same commands without initializing upstream
checkouts. When a registered checkout is present, the source check also verifies
its exact revision and tracked-file state.

## Runtime producers

The common/JVM producer accepts an explicit JSON input file:

```sh
node tools/build.mjs --inputs /absolute/path/to/build-inputs.json
```

The object maps native producer option names, without leading dashes, to absolute
paths. `output` selects the output directory. `build-work` selects a reusable
compiler workspace, and `tool-inputs` can select the directory containing verified
Maven build tools. The explicit build command downloads missing Maven tools and
checks their recorded sizes and SHA-256 hashes. Upstream source checkouts remain
read-only; the producer adapts owned source projections.

The ART producer accepts its existing grouped input configuration:

```sh
node tools/build-art.mjs --inputs /absolute/path/to/art-inputs.json
```

Its inputs include the pinned AIM, Klepton and GLES-header checkouts, host and
Java suppliers, Android component inputs and boot class path. The producer
returns the runtime supplier directory and corresponding source archive. The
[repository reference](../README.md) describes the individual input groups,
source-export options and reusable component suppliers.

## Distribution

MacLauncher Android supplies the pinned producer source used by
[MacLauncher Builds](https://github.com/procrastinine/maclauncher-builds).
MacLauncher Builds publishes selected runtime binaries and required notices in
binary-only release archives. Its tagged source tree contains the build recipes,
owned patches and pinned Android source. The original Android system image is
acquired separately. Ordinary runtime installation and play do not require this
source checkout or compiler tools.
