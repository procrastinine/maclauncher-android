# Native image preparation

`GuestNativeEntry.loadPrepared(runtime, file, soname, preparation)` supplies the
root image handle to `LongConsumer.accept` exactly once before native constructors.
The callback publishes the image's exports and class-loader ownership, completes
the linked future, and binds available JNI exports. A callback failure prevents
constructor execution and remains the load failure's original cause.

The returned handle must match the nonzero prepared handle. The constructor
future completes after the native load returns successfully. Recursive constructor
loads reuse the published image; incomplete dependency cycles retain their error.
Callbacks and constructors run outside the `NativeBinding` class monitor and the
Java loader's registry/image monitors.

The separate legacy load, construct and initialize entry points remain available.
This contract is independent of the native architecture and execution provider.
