# Android native library

The public library retains all 431 functions and both original symbol versions,
LIBANDROID and LIBANDROID_PLATFORM. exports.json records the original image's
export surface and library identity. generate.mjs matches those functions to the
actual built NDK, graphics, Looper and Choreographer carriers. Functions outside
those typed families retain the original Android implementation.

The launcher mounts the unchanged original library at
/system/lib64/libandroid-platform.so. The public library resolves that explicit
path and uses each original function's symbol version. Typed functions resolve
from their existing carrier handles. All handles stay retained until the public
library unloads; unresolved or recursive targets fail during initialization.

Assembly tail branches preserve all argument and result registers, stack
arguments and the application's return address. Callback registrations therefore
retain the authenticated application caller when entered through dlopen/dlsym.
The implementation does not replace the dynamic loader or change its lookup
rules. Unrecognised names retain the loader's normal missing-symbol result.

Normal builds call generateLibandroidFacade synchronously after building the
typed carriers and before linking this library. The generated C, assembly,
version map and routing inventory belong to the selected build output directory.
Refreshing exports.json is an explicit operation against a changed original
Android library, using the NDK's llvm-readelf.
