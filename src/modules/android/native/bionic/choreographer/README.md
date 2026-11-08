# Native Choreographer transport

All 14 functions of Android's NDK Choreographer API use the existing native
Looper and display clock. The original declarations are kept in
include/android/choreographer.h, and generate.mjs reads the normal compiler AST.
AChoreographer_getInstance, historically declared in C with an empty parameter
list, is emitted with its documented void argument list.

Frame, frame64, vsync and refresh callbacks keep their original calling thread,
arguments and application userdata. The display implementation embeds a copied
execution descriptor in each frame or refresh registration, and cancellation or
thread shutdown releases it with that native entry. A refresh callback is
identified by its function and userdata, independently of where it was
registered.

Callback entry reads the actual JavaVM environment without attaching a thread
and uses the authenticated image caller. Vsync callback data is borrowed for the
duration of the callback, and all six data getters go to the same native
implementation. The transport allocates no separate per-frame holder and keeps
the public void return values of the scheduling functions. Host and guest both
validate the complete argument layout before installation. The guest DSO is
libmaclauncher-choreographer.so.
