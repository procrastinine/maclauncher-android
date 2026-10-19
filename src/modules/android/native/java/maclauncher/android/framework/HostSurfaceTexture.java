package maclauncher.android.framework;

import android.graphics.SurfaceTexture;

/** Native producer ownership below Android's original SurfaceTexture methods. */
public final class HostSurfaceTexture implements HostSurface.Target {
    private static volatile boolean initialized;
    private long lease;
    private HostSurfaceTexture(){}
    public static long createBuffer(){
        var target=new HostSurfaceTexture();
        long handle=HostSurface.create(1,1,android.graphics.PixelFormat.RGBA_8888,target);
        target.lease=HostSurface.buffer(handle).lease;
        initialized=true;
        return handle;
    }
    public static long bufferLease(long handle){return HostSurface.buffer(handle).lease;}
    public static void resizeBuffer(long handle,int width,int height){HostSurface.buffer(handle).resize(width,height);}
    public boolean submit(int[] pixels,int width,int height,long frameId,long sourceTimeNanos){return submitCanvas(lease,pixels,width,height,sourceTimeNanos);}
    public void resize(int width,int height){}
    public static native long acquireBuffer(SurfaceTexture texture);
    public static void shutdown(){if(initialized)retireOwners();}
    private static native void retireOwners();
    private static native boolean submitCanvas(long lease,int[] pixels,int width,int height,long timestamp);
}
