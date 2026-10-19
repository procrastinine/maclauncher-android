package maclauncher.android.framework;

/** Supervisor-assigned Android identity; the macOS process credentials stay unchanged. */
public final class HostProcessIdentity {
    private HostProcessIdentity() {}
    public static void initializeIds(){initializeIdsNative();}
    public static int physicalPid(){return physicalPidNative();}
    public static synchronized void configure(int uid,int expectedHostUid) {
        configureNative(uid,expectedHostUid);
        initializeBinderNative();
    }
    private static native void configureNative(int uid,int expectedHostUid);
    private static native void initializeIdsNative();
    private static native int physicalPidNative();
    private static native void initializeBinderNative();
}
