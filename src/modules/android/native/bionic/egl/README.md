# Bionic EGL and GLES calls

EGL and GLES calls reach the host through a generated ARM64 carrier, a guest
library that uses the original NDK EGL and GLES declarations and the complete
ANGLE `GLES2/gl2ext_angle.h` declarations. Clang supplies parameter types and
widths. ANGLE additions follow the original NDK target
numbers, and sorted name indexes keep procedure lookup a binary search.

`eglGetProcAddress` returns guest procedures backed by the installed native
provider. The declaration snapshot records the hashes of its original header and
compiler extractor. `angle-declarations.mjs` regenerates that snapshot from an
NDK header directory with a Python interpreter.

GL debug callbacks and ANGLE blob-cache callbacks keep the original context
lifetime. The blob-cache setter passes the original validation before accepting
its native payload. Replacing or unregistering a callback, or destroying the
context, releases that payload, and callback invocations in progress keep their
own reference. Callback queries return the original guest function and user
values. Native callback entry uses the current physical thread and its actual
Java attachment state.
