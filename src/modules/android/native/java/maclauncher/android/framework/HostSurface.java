package maclauncher.android.framework;

import android.graphics.*;
import android.view.Surface;
import android.hardware.HardwareBuffer;
import android.os.Parcel;
import maclauncher.android.bridge.NativeSurface;
import java.lang.invoke.*;
import java.util.Objects;

/** Platform buffer ownership under the complete AOSP Surface implementation. */
public final class HostSurface {
    public interface Target {
        boolean submit(int[] pixels,int width,int height,long frameId,long sourceTimeNanos);
        /** A consumer can retain the producer's actual buffer without a color copy. */
        default int queueBuffer(long buffer,long frameId,long sourceTimeNanos,int dataSpace){return -38;}
        void resize(int width,int height);
        /** Allocate the native identity from this same producer's buffer owner. */
        default long createNativeSurface(int width,int height,int format){return NativeSurface.create(width,height,format);}
        /** Completion reports the output consumer's latch, independently of physical display. */
        default void onFrameCompleted(long frameId,java.util.function.LongConsumer completion){completion.accept(System.nanoTime());}
        default void close(){}
    }
    private static final VarHandle nativeObject;
    private static final MethodHandle assign;
    static {
        try{
            var lookup=MethodHandles.privateLookupIn(Surface.class,MethodHandles.lookup());
            nativeObject=lookup.findVarHandle(Surface.class,"mNativeObject",long.class);
            assign=lookup.findVirtual(Surface.class,"setNativeObjectLocked",MethodType.methodType(void.class,long.class));
        }catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}
    }
    private HostSurface(){}
    static final class Buffer implements AutoCloseable {
        final long lease;
        final Target target;
        private Bitmap bitmap;
        private int[] pixels;
        private Canvas locked;
        private Thread owner;
        private boolean closed;
        int width,height,format;
        long frame;
        boolean pending;
        final java.util.Set<java.util.function.LongConsumer> frames=java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>());
        Buffer(int width,int height,int format,Target target){
            this.width=width;this.height=height;this.format=format;this.target=target;
            lease=target==null?NativeSurface.create(width,height,format):target.createNativeSurface(width,height,format);
        }
        Buffer(long lease){int[] size=NativeSurface.leaseGeometry(lease);this.lease=lease;width=size[0];height=size[1];format=size[2];target=null;}
        void resize(int width,int height){synchronized(HostSurfaceControl.scene){
            if(this.width==width&&this.height==height)return;
            if(locked!=null)throw new IllegalStateException("Surface Canvas is locked");
            // A native EGL producer can select its actual config format after
            // this Java owner is created. Extent changes retain that format.
            NativeSurface.resize(lease,width,height,0);
            if(target!=null)target.resize(width,height);
            if(bitmap!=null)bitmap.recycle();bitmap=null;pixels=null;pending=false;this.width=width;this.height=height;
        }}
        void lock(Canvas canvas,Rect dirty){synchronized(HostSurfaceControl.scene){
            if(closed||locked!=null)throw new IllegalStateException("Surface Canvas is unavailable or locked");
            if(target==null)throw new IllegalStateException("Surface has no Canvas presentation owner");
            if(bitmap==null){bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);pixels=new int[Math.multiplyExact(width,height)];}
            canvas.setBitmap(bitmap);canvas.setMatrix(null);
            if(dirty!=null){if(!dirty.intersect(0,0,width,height))dirty.setEmpty();canvas.clipRect(dirty);}
            locked=canvas;owner=Thread.currentThread();
        }}
        void post(Canvas canvas){
            synchronized(HostSurfaceControl.scene){
                if(closed||locked!=canvas||owner!=Thread.currentThread())throw new IllegalStateException("Surface Canvas belongs to another lock");
                try{
                bitmap.getPixels(pixels,0,width,0,0,width,height);frame++;
                pending=!target.submit(pixels,width,height,frame,System.nanoTime());
                for(var listener:java.util.List.copyOf(frames))listener.accept(frame);
                }finally{canvas.setBitmap(null);locked=null;owner=null;}
            }
        }
        @Override public void close(){synchronized(HostSurfaceControl.scene){
            if(closed)return;closed=true;pending=false;
            try{if(locked!=null)locked.setBitmap(null);if(bitmap!=null)bitmap.recycle();}
            finally{locked=null;owner=null;bitmap=null;pixels=null;frames.clear();try{NativeSurface.release(lease);}finally{if(target!=null)target.close();}}
        }}
    }
    static Buffer buffer(long handle){return (Buffer)HostNativeHandle.state(handle);}
    static long create(int width,int height,int format,Target target){
        var buffer=new Buffer(width,height,format,target);long handle=0;
        try{handle=HostNativeHandle.create(buffer);NativeSurface.bindConsumer(buffer.lease,handle,HostNativeHandle.retainer(),HostNativeHandle.finalizer());return handle;}
        catch(Throwable failure){if(handle!=0)HostNativeHandle.release(handle);else buffer.close();throw failure;}
    }
    /** Only a host adapter assigns a native backing; AOSP owns generation state. */
    public static void bind(Surface surface,long lease){
        if(!NativeSurface.valid(lease))throw new IllegalArgumentException("Native Surface lease is unavailable");
        if((long)nativeObject.get(surface)!=0)throw new IllegalStateException("Surface is already bound");
        var buffer=new Buffer(lease);long handle=0;
        try{handle=HostNativeHandle.create(buffer);NativeSurface.bindConsumer(buffer.lease,handle,HostNativeHandle.retainer(),HostNativeHandle.finalizer());}
        catch(Throwable failure){if(handle!=0)HostNativeHandle.release(handle);else buffer.close();throw failure;}
        try{assign(surface,handle);}catch(Throwable failure){HostNativeHandle.release(handle);throw failure;}
    }
    /** Attach a supplied per-Surface buffer owner. The target owns presentation;
     * no window or software scene compositor is created by this binding. */
    public static void bind(Surface surface,int width,int height,int format,Target target){
        Objects.requireNonNull(surface);Objects.requireNonNull(target);
        if((long)nativeObject.get(surface)!=0)throw new IllegalStateException("Surface is already bound");
        long handle=create(width,height,format,target);
        try{assign(surface,handle);}catch(Throwable failure){HostNativeHandle.release(handle);throw failure;}
    }
    public static void assign(Surface surface,long handle){
        try{assign.invokeExact(surface,handle);}catch(RuntimeException|Error failure){throw failure;}catch(Throwable failure){throw new IllegalStateException(failure);}
    }
    public static long nativeLease(Surface surface){long handle=(long)nativeObject.get(surface);return handle==0?0:buffer(handle).lease;}
    /** ANativeWindow_toSurface creates another original Surface over this consumer. */
    public static Surface fromNativeHandle(long handle){
        long retained=HostNativeHandle.retain(handle);
        try{var surface=new Surface();assign(surface,retained);return surface;}
        catch(Throwable failure){HostNativeHandle.release(retained);throw failure;}
    }
    /** Native producers retain this same Buffer through HostNativeHandle. */
    public static int videoTarget(long handle){var target=buffer(handle).target;return target==null?0:target instanceof HostSurfaceTexture?1:2;}
    public static boolean submitVideoPixels(long handle,int[] pixels,int width,int height,long timestamp){
        var target=buffer(handle);if(target.target==null)return false;
        synchronized(HostSurfaceControl.scene){
        if(target.closed||target.locked!=null)return false;
        if(target.width!=width||target.height!=height)target.resize(width,height);
        if(target.bitmap==null)target.bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);
        target.bitmap.setPixels(pixels,0,width,0,0,width,height);target.pixels=pixels;target.frame++;
        target.pending=!target.target.submit(pixels,width,height,target.frame,timestamp);
        for(var listener:java.util.List.copyOf(target.frames))listener.accept(target.frame);
        return !target.pending;
        }
    }
    /** Copy the last posted Canvas image; an active producer lock is never read. */
    static int copyCanvasInto(Surface surface,Rect requested,Bitmap destination){synchronized(HostSurfaceControl.scene){
        long handle=(long)nativeObject.get(surface);if(handle==0)return android.view.PixelCopy.ERROR_SOURCE_INVALID;
        Buffer source=buffer(handle);if(source.bitmap==null)return -1;
        if(source.frame==0)return android.view.PixelCopy.ERROR_SOURCE_NO_DATA;
        Rect region=requested==null||requested.isEmpty()?new Rect(0,0,source.width,source.height):new Rect(requested);
        if(!region.intersect(0,0,source.width,source.height))return android.view.PixelCopy.ERROR_SOURCE_INVALID;
        Bitmap snapshot=Bitmap.createBitmap(source.pixels,source.width,source.height,Bitmap.Config.ARGB_8888);
        Canvas canvas=new Canvas(destination);
        try{canvas.drawBitmap(snapshot,region,new Rect(0,0,destination.getWidth(),destination.getHeight()),new Paint(Paint.FILTER_BITMAP_FLAG));return android.view.PixelCopy.SUCCESS;}finally{canvas.setBitmap(null);snapshot.recycle();}
    }}
    public static long nativeCreateFromSurfaceControl(long control){return HostSurfaceControl.acquireBuffer(control);}
    public static long nativeCreateFromSurfaceTexture(SurfaceTexture texture){return HostSurfaceTexture.acquireBuffer(Objects.requireNonNull(texture));}
    public static long nativeGetFromBlastBufferQueue(long current,long queue){long handle=HostBufferQueue.bufferHandle(queue);return handle==current?current:HostNativeHandle.retain(handle);}
    public static long nativeGetFromSurfaceControl(long current,long control){long handle=HostSurfaceControl.bufferHandle(control);return handle==current?current:HostNativeHandle.retain(handle);}
    public static long nativeLockCanvas(long handle,Canvas canvas,Rect dirty){buffer(handle).lock(canvas,dirty);return HostNativeHandle.retain(handle);}
    public static void nativeUnlockCanvasAndPost(long handle,Canvas canvas){buffer(handle).post(canvas);}
    public static void nativeRelease(long handle){HostNativeHandle.release(handle);}
    public static void nativeWriteToParcel(long handle,Parcel parcel){if(handle!=0)buffer(handle);HostObjectParcel.writeNativeHandle(parcel,handle);}
    public static long nativeReadFromParcel(long current,Parcel parcel){long next=HostObjectParcel.readNativeHandle(parcel,Buffer.class);if(current!=0)HostNativeHandle.release(current);return next;}
    public static int nativeAttachAndQueueBufferWithColorSpace(long handle,HardwareBuffer hardware,int colorSpace){
        long source=HostNativeHandle.retain(HostHardwareBuffer.handle(Objects.requireNonNull(hardware)));
        try{synchronized(HostSurfaceControl.scene){
            Buffer target=buffer(handle);if(target.closed)return -19;if(target.locked!=null)return -16;
            int width=HostHardwareBuffer.nGetWidth(source),height=HostHardwareBuffer.nGetHeight(source);
            if(target.target!=null){
                ColorSpace.Named[] spaces=ColorSpace.Named.values();if(colorSpace<0||colorSpace>=spaces.length)return -22;
                int result=target.target.queueBuffer(source,target.frame+1,System.nanoTime(),ColorSpace.get(spaces[colorSpace]).getDataSpace());
                if(result!=-38){if(result==0){target.frame++;target.pending=false;for(var listener:java.util.List.copyOf(target.frames))listener.accept(target.frame);}return result;}
            }
            if(target.target==null&&colorSpace==ColorSpace.Named.SRGB.ordinal()){
                if(target.width!=width||target.height!=height)target.resize(width,height);
                int result=HostHardwareBuffer.present(source,target.lease);if(result==0){target.frame++;target.pending=false;for(var listener:java.util.List.copyOf(target.frames))listener.accept(target.frame);}return result;
            }
            // The producer supplies actual pixels; composition retains its existing
            // Canvas color conversion and presentation path.
            int[] pixels=HostHardwareBuffer.readPixels(source);
            if(colorSpace!=ColorSpace.Named.SRGB.ordinal()){
                ColorSpace.Named[] names=ColorSpace.Named.values();if(colorSpace<0||colorSpace>=names.length)return -22;
                var conversion=ColorSpace.connect(ColorSpace.get(names[colorSpace]),ColorSpace.get(ColorSpace.Named.SRGB));
                float[] channels=new float[3];for(int i=0;i<pixels.length;i++){int color=pixels[i];channels[0]=Color.red(color)/255f;channels[1]=Color.green(color)/255f;channels[2]=Color.blue(color)/255f;conversion.transform(channels);pixels[i]=Color.argb(Color.alpha(color),Math.round(channels[0]*255),Math.round(channels[1]*255),Math.round(channels[2]*255));}
            }
            if(target.width!=width||target.height!=height)target.resize(width,height);
            if(target.target==null){long converted=HostHardwareBuffer.allocate(width,height,HardwareBuffer.RGBA_8888,HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE);try{HostHardwareBuffer.writePixels(converted,pixels);int result=HostHardwareBuffer.present(converted,target.lease);if(result==0){target.frame++;target.pending=false;for(var listener:java.util.List.copyOf(target.frames))listener.accept(target.frame);}return result;}finally{HostNativeHandle.release(converted);}}
            if(target.bitmap==null)target.bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);
            target.bitmap.setPixels(pixels,0,width,0,0,width,height);target.pixels=pixels;target.frame++;
            target.pending=!target.target.submit(pixels,width,height,target.frame,System.nanoTime());
            for(var listener:java.util.List.copyOf(target.frames))listener.accept(target.frame);
            return 0;
        }}finally{HostNativeHandle.release(source);}
    }
    public static boolean nativeIsValid(long handle){return NativeSurface.valid(buffer(handle).lease);}
    public static boolean nativeIsConsumerRunningBehind(long handle){return buffer(handle).pending;}
    public static int nativeGetWidth(long handle){return buffer(handle).width;}
    public static int nativeGetHeight(long handle){return buffer(handle).height;}
    public static long nativeGetNextFrameNumber(long handle){return buffer(handle).frame+1;}
    public static void nativeAllocateBuffers(long handle){buffer(handle);}
    public static void nativeDestroy(long handle){buffer(handle).close();}
    public static int nativeSetScalingMode(long handle,int mode){buffer(handle);return mode>=0&&mode<=3?0:-22;}
    public static int nativeForceScopedDisconnect(long handle){buffer(handle);return 0;}
    public static int nativeSetSharedBufferModeEnabled(long handle,boolean enabled){buffer(handle);return enabled?-38:0;}
    public static int nativeSetAutoRefreshEnabled(long handle,boolean enabled){buffer(handle);return enabled?-38:0;}
    public static int nativeSetFrameRate(long handle,float rate,int compatibility,int strategy){buffer(handle);return rate>=0&&Float.isFinite(rate)?0:-22;}
}
