package maclauncher.android.bridge;

/** Manual location sheet in the Android host's existing native window. */
public final class NativeLocation {
    // Host loads the selected graphics library before application callbacks.
    public static native void request(long requestId);
    public static native void cancel(long requestId);
    private NativeLocation() {}
}
