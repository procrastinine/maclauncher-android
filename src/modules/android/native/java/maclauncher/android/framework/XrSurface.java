package maclauncher.android.framework;

import android.graphics.SurfaceTexture;
import android.view.Surface;
import maclauncher.android.bridge.NativeSurface;

/** An OpenXR producer uses the original Surface and its shared native window. */
public final class XrSurface implements AutoCloseable {
    private final SurfaceTexture consumer;
    private final Surface surface;
    private long window;

    private XrSurface(int width,int height){
        if(width<=0||height<=0)throw new IllegalArgumentException("Projection surface dimensions must be positive");
        var texture=new SurfaceTexture(false);
        Surface producer=null;
        long identity=0;
        try{
            texture.setDefaultBufferSize(width,height);
            producer=new Surface(texture);
            identity=NativeSurface.acquire(producer);
            if(identity==0)throw new IllegalStateException("Projection surface has no native window");
        }catch(Throwable failure){
            if(identity!=0)NativeSurface.releaseWindow(identity);
            try{if(producer!=null)producer.release();}finally{texture.release();}
            throw failure;
        }
        consumer=texture;surface=producer;window=identity;
    }
    public static XrSurface create(int width,int height){return new XrSurface(width,height);}
    public synchronized Surface surface(){if(window==0)throw new IllegalStateException("Projection surface is closed");return surface;}
    public synchronized long identity(){return window;}
    @Override public synchronized void close(){
        long identity=window;if(identity==0)return;window=0;
        try{NativeSurface.releaseWindow(identity);}finally{try{surface.release();}finally{consumer.release();}}
    }
}
