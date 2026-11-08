# AAudio native carrier

The AAudio carrier is a generated guest library that forwards the AAudio API to the host. It exports every AAudio declaration in the original NDK header. Typed guest functions pass their arguments to the existing macOS audio backend, which keeps ownership of stream and builder handles. AAudio result codes, adopted stream properties and unavailable-device errors come from the backend.

Data, error and presentation-end callbacks enter the original guest invocation boundary with the image that registered them. The current physical thread supplies its actual JNI environment when it is attached and stays detached otherwise. The backend stores callback descriptors in the builder and freezes a copy in each opened stream, so reusing or deleting a builder leaves existing streams unchanged. Closing a stream keeps the original feeder join before callback storage is released. Nothing is allocated per buffer or in callback setters.

`generate.mjs` reads the compiler-produced host AST and the original `aaudio/AAudio.h` declarations. Both generated sides validate every transport target and argument size before installing their tables. The guest library uses the `libaaudio.so` SONAME and keeps the complete public function set.
