# Building

This repository's source tools need Node.js 24 or later and Git, while native
runtime builds need macOS on Apple Silicon plus the compiler, JDK, SDK and
dependency inputs named in their input configuration. Two build scripts, called
producers, turn those inputs into runtime packages: `tools/build.mjs` builds the
common/JVM package and `tools/build-art.mjs` builds the ART package.

## Source checks

```sh
npm run check
node --test tests/android-source-check.test.mjs tests/android-source-owner.test.mjs tests/android-art-package-inputs.test.mjs
node tools/build-art.mjs --check
```

These commands check source imports, upstream revisions, source ownership and
package input requirements. They neither download nor compile runtime
components. The GitHub source workflow runs the same commands without
initializing the upstream checkouts. When a registered checkout is present, the
source check also verifies its exact revision and the state of its tracked
files.

## Tests

The full test suite runs with the pinned AIM checkout initialized:

```sh
git submodule update --init src/external/aim-custom
npm test
```

The native fixtures use Clang, Rust/Cargo and a JDK. The memory-policy probe
builds offline, so Cargo's dependency cache must already contain `libc`.
Tests need local process creation and listening sockets. Sandboxes that suppress
file permission bits cannot exercise the native-publication assertions.
Optional integration tests state their required runtime inputs in their skip
messages. The default suite does not launch the app or submit GPU work.

## Runtime producers

The common/JVM producer reads an explicit JSON input file:

```sh
node tools/build.mjs --inputs /absolute/path/to/build-inputs.json
```

The JSON object maps native producer option names, without leading dashes, to
absolute paths. `output` selects the output directory, `build-work` selects a
reusable compiler workspace, and `tool-inputs` can select a directory of
verified Maven build tools. The build command downloads any missing Maven tools
and checks their recorded sizes and SHA-256 hashes. Upstream source checkouts
stay read-only, and the producer applies its adaptations to its own staged
copies of the sources.

The ART producer reads its grouped input configuration:

```sh
node tools/build-art.mjs --inputs /absolute/path/to/art-inputs.json
```

Its inputs include the AIM, Klepton and GLES-header checkouts at their recorded
revisions, the `host` and `java` input groups, the Android component inputs and
the boot class path. The producer returns the ART supplier, a directory of ART
runtime components with a manifest from which the ART package is assembled,
together with the corresponding source archive. The
[source and runtime reference](REFERENCE.md) describes each input group, the
source-export options and the reusable prebuilt components.

The common native-memory, ART and standalone AIM integrations keep separate sets
of source revisions. Each uses the framework, JNI, CPU and native-library
revisions recorded by its own producer, so a standalone AIM revision does not
replace the revision used by ART or by the common native-memory integration. Both execution bindings compile the shared Java
adapters against their own runtime declarations, and JVM-only adapters stay in
their separate source group.

The compiler workspace reuses unchanged native objects and supplier components.
Framework assembly, application DEX conversion, native compilation and ART boot
image generation each have their own cache key. MacLauncher Builds packages the
completed supplier outputs with their manifests and flat `licenses/` directories. Build
directories and source archives stay separate from installed runtime packages.

## Distribution

[MacLauncher Builds](https://github.com/procrastinine/maclauncher-builds) runs
these producers from an exact commit of this repository. It publishes selected
runtime binaries and licence texts in binary-only release archives,
and its tagged source tree contains the build recipes, MacLauncher's patches and
this repository at the commit that was built. The original
Android system image is downloaded separately. Installing a runtime and playing
a game need neither this source checkout nor any compiler tools.

Shared source helpers and graphics declarations come from the exact `sources/maclauncher` Gitlink. Initialize that submodule before running the Node suite or source producers. Android runtime tests and fixtures live in this repository.

Explicit ART supplier reassembly accepts a flat `inputs.licenses` directory with its component index and licence texts. The directory retains its identity and content when the supplier moves.

Source checks read the shared helpers at the `sources/maclauncher` Gitlink. Initialize that exact source owner before checking imports:

```sh
git submodule update --init --depth 1 -- sources/maclauncher
npm run check
```
