package maclauncher.android.bridge;

import android.app.Activity;
import android.view.*;
import maclauncher.android.framework.*;
import java.util.Objects;

/** One pending native transport beneath Android's own ViewRoot drawing and traversal scheduler. */
public final class CanvasFrameSource implements AutoCloseable {
    public static final int IDLE=0,SUBMITTED=1,BUSY=2;
    public interface Backend {
        boolean submit(int[] pixels,int width,int height,long frameId,long sourceTimeNanos);
        void resize(int width,int height);
    }
    private final Thread owner=Thread.currentThread();
    private final Backend backend;
    private final SurfaceControl root;
    private int[] pendingPixels;
    private int width,height;
    private long frameId,sourceTime,drawn,submitted,busy;
    private long latchedFrameId,latchTimeNanos;
    private boolean pending,closed,pumping;
    public CanvasFrameSource(Activity activity,Backend backend,int width,int height){
        this.backend=Objects.requireNonNull(backend);HostActivity.owner(Objects.requireNonNull(activity));checkThread();
        if(!android.os.Looper.getMainLooper().isCurrentThread())throw new IllegalStateException("Canvas requires the main window Looper");
        dimensions(width,height);this.width=width;this.height=height;backend.resize(width,height);
        root=HostSurfaceControl.rootSurfaceControl("Android application window",width,height,new HostSurface.Target(){
            @Override public boolean submit(int[] pixels,int w,int h,long frame,long time){return enqueue(pixels,w,h,frame,time);}
            @Override public void resize(int w,int h){backend.resize(w,h);}
            @Override public void onFrameCompleted(long frame,java.util.function.LongConsumer completion){
                checkOpen();if(frame>latchedFrameId)throw new IllegalStateException("Canvas frame has not entered its output consumer");
                completion.accept(latchTimeNanos);
            }
        });
        try{HostWindowManager.attachOutput(activity,root,width,height);}
        catch(Throwable failure){root.release();closed=true;throw failure;}
    }
    private static void dimensions(int width,int height){if(width<1||height<1||width>8192||height>8192||(long)width*height>16777216)throw new IllegalArgumentException("Canvas framebuffer dimensions exceed the supported range");}
    private void checkThread(){if(Thread.currentThread()!=owner)throw new IllegalStateException("Canvas frames require the window owner thread");}
    private void checkOpen(){checkThread();if(closed)throw new IllegalStateException("Canvas frame source is closed");}
    private boolean enqueue(int[] pixels,int w,int h,long frame,long time){
        checkOpen();if(w!=width||h!=height)throw new IllegalArgumentException("Canvas frame does not match its native output");int count=Math.multiplyExact(w,h);
        if(pendingPixels==null||pendingPixels.length!=count)pendingPixels=new int[count];
        System.arraycopy(pixels,0,pendingPixels,0,count);frameId=frame;sourceTime=time;pending=true;drawn++;
        // This consumer now owns the copied image for this Android frame.
        // A later accepted image can subsume it before Mac presentation. Android
        // transaction callbacks report this latch, independently of display.
        latchedFrameId=frame;latchTimeNanos=System.nanoTime();
        // Surface unlock has transferred its pixels into this bounded latest-frame
        // owner. GPU pressure is retried by the existing native display pump.
        return true;
    }
    public void resize(int w,int h){checkOpen();if(pumping)throw new IllegalStateException("Cannot resize during submission");dimensions(w,h);if(w==width&&h==height)return;HostSurfaceControl.resizeRoot(root,w,h);width=w;height=h;pending=false;pendingPixels=null;HostWindowManager.resizeOutput(w,h);}
    public int hostPump(){return pump(System.nanoTime());}
    public int pump(long ignoredNow){checkOpen();if(pumping)throw new IllegalStateException("Reentrant Canvas submission");HostSurfaceControl.refreshNativePresentation(root);if(!pending)return IDLE;pumping=true;try{if(!backend.submit(pendingPixels,width,height,frameId,sourceTime)){busy++;return BUSY;}pending=false;submitted++;return SUBMITTED;}finally{pumping=false;}}
    public long[] statistics(){checkThread();return new long[]{drawn,submitted,busy,pendingPixels==null?0:(long)pendingPixels.length*4,pending?1:0};}
    @Override public void close(){checkThread();if(pumping)throw new IllegalStateException("Cannot close during submission");if(closed)return;closed=true;pending=false;pendingPixels=null;try{HostWindowManager.detachOutput();}finally{root.release();}}
}
