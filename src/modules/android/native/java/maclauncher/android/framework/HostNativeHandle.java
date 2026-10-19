package maclauncher.android.framework;

/** NativeAllocationRegistry backing for platform objects with Java-owned state.
 * Nonreused identities refer to retained native records, never foreign ABI
 * pointers. Retain keeps the same owner alive; release retires only that owner. */
public final class HostNativeHandle {
    private HostNativeHandle() {}
    public static native long create(AutoCloseable state);
    public static native Object state(long handle);
    public static native long retain(long handle);
    public static native void release(long handle);
    public static native long finalizer();
    /** The same registry's retain operation for native platform leases. */
    public static native long retainer();
}
