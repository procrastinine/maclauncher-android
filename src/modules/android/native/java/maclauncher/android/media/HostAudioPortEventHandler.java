package maclauncher.android.media;

/** Native route subscription for Android's package-private original dispatcher. */
public final class HostAudioPortEventHandler {
    private HostAudioPortEventHandler(){}
    static {AudioHost.initialize();}
    public static native void native_setup(Object owner,Object reference);
    public static native void native_finalize(Object owner);
}
