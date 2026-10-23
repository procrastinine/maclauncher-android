# Bionic EGL and GLES calls

The generated ARM64 carrier uses the original NDK EGL/GLES declarations and
the complete pinned ANGLE `GLES2/gl2ext_angle.h` declarations. Clang supplies
parameter types and widths. ANGLE additions follow the original NDK target
numbers; sorted name indexes retain binary procedure lookup.

`eglGetProcAddress` returns guest procedures backed by the installed native
provider. The declaration snapshot records its original header and compiler
extractor hashes. `angle-declarations.mjs` regenerates that snapshot with an NDK
header directory and Python interpreter.

GL debug callbacks and ANGLE blob-cache callbacks retain the original context
lifetime. The blob-cache setter passes original validation before accepting
its native payload. Replacement, unregister and context destruction release
that payload; active callback invocations retain their own reference. Callback
queries retain the original guest function and user values. Native callback
entry uses the current physical thread and its actual Java attachment state.
