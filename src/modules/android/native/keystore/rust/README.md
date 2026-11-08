# KeyStore Rust build

This native build compiles the original Android KeyStore service, legacy store,
Binder Rust facade, AIDL interfaces, flag libraries and their dependencies. The
original Blueprint declarations select the complete dependency graph, including
inherited defaults and Android target dependencies. Cargo drives the macOS build
and locks registry packages to `Cargo.lock`.

`buildKeyStoreRust` accepts exported source roots, the shared native foundation,
Binder and KeyMint AIDL outputs, the native HAL adaptation, the Aconfig and
Python executables, an explicit Rust toolchain and an output directory. It
returns the optimized static Rust library and the two native crypto wrapper
archives for the final KeyStore library. The supported toolchain is Rust 1.93
with an Apple Silicon standard library that supports macOS 13.

Logging and base utilities come from the existing Binder library, and
cryptography comes from the canonical BoringSSL libraries. The Rust build adds
the original inline wrappers and KeyStore crypto helpers without another crypto
engine or logging library. System property access, clocks and HAL discovery use
the same native implementations as other Android components.

Process entry accepts the platform process's database directory and returns to
its Looper after the original service registration. The original RPC server
manages its connection workers. The software KeyMint adaptation reports its
actual security level. Source exports are regenerated together, and compiler
artifacts are kept separately for incremental builds.

The source catalog records the exact revision of every AOSP input. Registry version and feature metadata
are read from every original crate manifest and Blueprint file. Framework flags
use the original release inheritance and Aconfig generator. Source acquisition
is a separate step: the generator neither fetches Git sources nor inspects
applications. Build verification belongs under `tests/`.
