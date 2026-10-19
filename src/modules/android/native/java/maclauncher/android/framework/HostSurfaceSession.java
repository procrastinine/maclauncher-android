package maclauncher.android.framework;

/** Local compositor connection lifetime; the public session remains AOSP. */
public final class HostSurfaceSession {
    private HostSurfaceSession(){}
    public static long nativeCreate(){return HostNativeHandle.create(()->{});}
    public static void nativeDestroy(long handle){HostNativeHandle.release(handle);}
}
