# KeyStore Rust build

The native KeyStore supplier builds the original Android service, legacy store,
Binder Rust facade, AIDL interfaces, flag libraries and their dependencies. The
original Blueprint declarations select the complete dependency graph, including
inherited defaults and Android target dependencies. Cargo provides the macOS
build driver and locks registry packages to `Cargo.lock`.

`buildKeyStoreRust` accepts exported source roots, the shared native foundation,
Binder and KeyMint AIDL outputs, the native HAL adaptation, the Aconfig and Python
executables, an explicit Rust toolchain, and an output directory. It returns the
optimized static Rust library and the two native crypto wrapper archives for the
final KeyStore library. The supported toolchain is Rust 1.93 with an Apple Silicon
standard library supporting macOS 13.

The existing Binder library owns logging and base utilities. The canonical
BoringSSL libraries own cryptography. The Rust build adds the original inline
wrappers and KeyStore crypto helpers, with no additional crypto engine or logging
library. System property access, clocks and HAL discovery use the same native
owners as other Android components.

Process entry accepts the platform process's database directory and returns to
its Looper after original service registration. The original RPC server owns its
connection workers. The software KeyMint adaptation reports its actual security
level. Source exports are regenerated together; compiler artifacts are retained
separately for incremental builds.

The source catalog pins all AOSP inputs. Registry version and feature metadata is
read from every original crate manifest and Blueprint file. Framework flags use
the original release inheritance and Aconfig generator. Source acquisition is
separate from generation; the generator does not fetch Git sources or inspect
applications. Build verification belongs under `tests/`.
