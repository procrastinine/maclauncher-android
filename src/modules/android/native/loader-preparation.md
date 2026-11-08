# Native image preparation

`GuestNativeEntry.loadPrepared(runtime, file, soname, preparation)` passes the
root image handle to `LongConsumer.accept` exactly once, before native
constructors run. The callback publishes the image's exports and class-loader
ownership, completes the linked future and binds the available JNI exports. If
the callback fails, no constructor runs, and the callback's failure stays the
original cause of the load failure.

The returned handle must match the nonzero prepared handle. The constructor
future completes after the native load returns successfully. Loads made
recursively from constructors reuse the published image, and incomplete
dependency cycles keep their error. Callbacks and constructors run outside the
`NativeBinding` class monitor and the Java loader's registry and image monitors.

The separate legacy load, construct and initialize entry points remain
available. This contract does not depend on the native architecture or the
execution provider.
