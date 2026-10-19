package maclauncher.android.net;

/** Session policy shared by the guest resolver and embedded WebKit. */
public final class HostNetworkPolicy {
    private static volatile boolean configured;
    private HostNetworkPolicy(){}
    public static synchronized void configure(boolean allowAds){configureNative(allowAds);configured=true;}
    /** JVM bootstrap can resolve its own names before an Android session exists. */
    public static boolean hostAllowed(String host){return !configured||hostAllowedNative(host);}
    private static native void configureNative(boolean allowed);
    private static native boolean hostAllowedNative(String host);
}
