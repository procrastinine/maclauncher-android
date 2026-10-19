package maclauncher.android.framework;

/** IncFS queries on the host's ordinary, completely downloaded filesystems. */
public final class HostIncrementalFilesystem {
    private HostIncrementalFilesystem(){}
    // macOS has no IncFS mount or IncrementalService. These are filesystem
    // observations; the original APK verifier still checks sidecar and APK signatures.
    public static boolean nativeIsEnabled(){return false;}
    public static boolean nativeIsV2Available(){return false;}
    public static boolean nativeIsIncrementalPath(String path){return false;}
    public static boolean nativeIsIncrementalFd(int descriptor){return false;}
    public static byte[] nativeUnsafeGetFileSignature(String path){return null;}
}
