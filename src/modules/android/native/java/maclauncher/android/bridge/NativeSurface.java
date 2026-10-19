package maclauncher.android.bridge;
/** Owned native-window leases. Class loading creates no GPU work or window. */
public final class NativeSurface {
    static {String file=System.getProperty("klepton.graphics");if(file==null)throw new UnsupportedOperationException("Native Android surface bridge is unavailable");System.load(file);}
    private NativeSurface(){}
    public static native long create(int width,int height,int format);
    public static native long createComposition(int width,int height,int format,NativeComposition.Layer producer);
    public static native boolean valid(long lease);
    public static native void bindConsumer(long lease,long nativeHandle,long retainer,long finalizer);
    public static native boolean externallyPresented(long lease);
    public static native long externalGeneration();
    public static native void resize(long lease,int width,int height,int format);
    public static native void release(long lease);
    public static native long[] statistics();
    public static native long acquire(android.view.Surface surface);
    public static native void releaseWindow(long window);
    public static native int[] geometry(long window);
    public static native int[] leaseGeometry(long lease);
    public static native long storageId(long window);
    public static native boolean storageAlive(long id);
    public interface PixelCopyCallback {void complete(int status,int[] pixels);}
    public static native long requestPixelCopy(android.view.Surface source,int width,int height,PixelCopyCallback callback);
    public static native long requestPixelCopyRegion(android.view.Surface source,int left,int top,int right,int bottom,int width,int height,PixelCopyCallback callback);
    public static native void cancelPixelCopy(long requestId);
    public static native void install(String runtime);
    public static native void configureGraphics(String graphics);
    public static native void configureCache(String directory);
    public static native void run(android.view.View view,android.app.Activity activity,String title,int width,int height);
    public static native void shutdownDisplayCallbacks();
}
