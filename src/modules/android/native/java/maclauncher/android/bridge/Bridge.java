package maclauncher.android.bridge;

/** Shared Klepton filesystem configuration and process memory information. */
public final class Bridge {
    private Bridge() {}
    static { System.load(System.getProperty("klepton.bridge")); }
    public static void configure(String assets,String files){
        maclauncher.android.framework.HostContext.hostFilesDirectory(files);
        configureNative(assets,files);
    }
    private static native void configureNative(String assets,String files);
    public static native void memoryInfo(long[] values);
}
