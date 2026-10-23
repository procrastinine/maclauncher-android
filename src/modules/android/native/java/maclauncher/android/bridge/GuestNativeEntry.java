package maclauncher.android.bridge;
/** Defined under the original APK loader so JNI FindClass uses that namespace. */
public final class GuestNativeEntry {
    private GuestNativeEntry(){}
    public static native long load(String runtime,String file,String soname,boolean initializeJni);
    /** Publishes the prepared image to its Java owner before ELF constructors run. */
    public static native long loadPrepared(String runtime,String file,String soname,java.util.function.LongConsumer preparation);
    public static native void initialize(long image);
    public static native void construct(long image);
    public static native void reject(long image);
    public static native long createActivity(long image,String entry,android.app.NativeActivity activity,byte[][] paths,int sdk,byte[] savedState);
    public static native void activityContentRect(long handle,int x,int y,int width,int height);
    public static native byte[] activityEvent(long handle,int event,long value);
    public static native void pumpActivity(long handle);
}
