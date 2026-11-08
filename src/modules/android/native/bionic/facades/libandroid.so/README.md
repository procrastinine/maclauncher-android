# Android native library

Android's public native library, `libandroid.so`, keeps all 431 functions and both original symbol versions,
LIBANDROID and LIBANDROID_PLATFORM. exports.json records the original image's
export surface and library identity. generate.mjs matches those functions to the
actual built NDK, graphics, Looper and Choreographer carriers (generated guest
libraries that forward those APIs to the host), and functions
outside those typed families keep the original Android implementation.

The launcher mounts the unchanged original library at
/system/lib64/libandroid-platform.so. The public library resolves that explicit
path and uses each original function's symbol version, while typed functions
resolve from their existing carrier handles. All handles stay open until the
public library unloads, and unresolved or recursive targets fail during
initialization.

Assembly tail branches preserve all argument and result registers, stack
arguments and the application's return address, so callback registrations
entered through dlopen/dlsym keep the authenticated application caller. The
implementation does not replace the dynamic loader or change its lookup rules,
and unrecognized names give the loader's normal missing-symbol result.

Normal builds call generateLibandroidFacade synchronously after building the
typed carriers and before linking this library. The generated C, assembly,
version map and routing inventory belong in the selected build output directory.
Refreshing exports.json is an explicit step, run against a changed original
Android library with the NDK's llvm-readelf.
