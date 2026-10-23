# AAudio native carrier

The carrier exports the complete AAudio declarations in the original NDK header. Typed guest functions carry their arguments to the existing macOS audio backend; stream and builder handles retain that backend's ownership. AAudio result codes, adopted stream properties and unavailable-device errors come from the backend.

Data, error and presentation-end callbacks enter the original guest invocation boundary with their registration's image identity. The current physical thread supplies its actual JNI environment when attached and remains detached otherwise. The backend stores callback descriptors in the builder and freezes a copy in each opened stream. Reusing or deleting a builder leaves existing streams unchanged. Stream close retains the original feeder join before releasing callback storage. No allocation occurs per buffer or in callback setters.

`generate.mjs` consumes the compiler-produced host AST and the original `aaudio/AAudio.h` declarations. Both generated sides validate every transport target and argument size before installing their tables. The guest library uses the `libaaudio.so` SONAME and retains the complete public function set.
