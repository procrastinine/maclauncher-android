# Native Choreographer transport

The complete 14-function Android NDK Choreographer API uses the existing native
Looper and display clock. Original declarations are retained in
include/android/choreographer.h; generate.mjs reads the normal compiler AST.
The historical C empty-parameter declaration of AChoreographer_getInstance is
emitted with its documented void argument list.

Frame, frame64, vsync and refresh callbacks retain their original calling thread,
arguments and application userdata. The display owner embeds a copied execution
descriptor in each frame or refresh registration; cancellation and thread
retirement release it with that native entry. Refresh identity uses the callback
function and userdata, independently of the registration callsite.

Callback entry reads the actual JavaVM environment without attaching a thread and
uses the authenticated image caller. Vsync callback data remains borrowed for the
duration of that callback; all six data getters route to the same native owner.
The transport allocates no separate per-frame holder and preserves public void
scheduling return values. Host and guest validate the complete argument layout
before installation. The guest DSO is libmaclauncher-choreographer.so.
