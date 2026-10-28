package maclauncher.android.framework;

import android.graphics.GraphicBuffer;
import android.hardware.HardwareBuffer;
import android.hardware.SyncFence;
import android.media.Image;
import android.media.ImageReader;
import android.view.Surface;
import java.lang.invoke.*;
import java.lang.reflect.Array;
import java.nio.ByteBuffer;
import java.util.*;

/** Buffer-consumer operations below the original ImageReader and Image objects. */
public final class HostImageReader {
    private HostImageReader(){}
    private static final VarHandle context,imageBuffer,imageTime,imageDataSpace,imageTransform,imageScaling;
    private static final Class<?> surfaceImage,surfacePlane;
    private static final MethodHandle imagePlaneConstructor,surfacePlaneConstructor,postEvent,imageReader;
    static {try{
        var lookup=MethodHandles.lookup();var reader=MethodHandles.privateLookupIn(ImageReader.class,lookup);
        context=reader.findVarHandle(ImageReader.class,"mNativeContext",long.class);
        postEvent=reader.findStatic(ImageReader.class,"postEventFromNative",MethodType.methodType(void.class,Object.class));
        surfaceImage=Class.forName("android.media.ImageReader$SurfaceImage",false,ImageReader.class.getClassLoader());
        var image=MethodHandles.privateLookupIn(surfaceImage,lookup);
        imageReader=image.findVirtual(surfaceImage,"getReader",MethodType.methodType(ImageReader.class));
        imageBuffer=image.findVarHandle(surfaceImage,"mNativeBuffer",long.class);
        imageTime=image.findVarHandle(surfaceImage,"mTimestamp",long.class);
        imageDataSpace=image.findVarHandle(surfaceImage,"mDataSpace",int.class);
        imageTransform=image.findVarHandle(surfaceImage,"mTransform",int.class);
        imageScaling=image.findVarHandle(surfaceImage,"mScalingMode",int.class);
        surfacePlane=Class.forName("android.media.ImageReader$SurfaceImage$SurfacePlane",false,ImageReader.class.getClassLoader());
        surfacePlaneConstructor=MethodHandles.privateLookupIn(surfacePlane,lookup).findConstructor(surfacePlane,
            MethodType.methodType(void.class,surfaceImage,int.class,int.class,ByteBuffer.class));
        imagePlaneConstructor=MethodHandles.privateLookupIn(ImageReader.ImagePlane.class,lookup).findConstructor(ImageReader.ImagePlane.class,
            MethodType.methodType(void.class,int.class,int.class,ByteBuffer.class));
    }catch(ReflectiveOperationException failure){throw new ExceptionInInitializerError(failure);}}
    private static RuntimeException failure(Throwable error){if(error instanceof RuntimeException value)return value;if(error instanceof Error value)throw value;return new IllegalStateException(error);}
    private static final class Frame {
        final long buffer,timestamp;final int dataSpace;boolean planesLocked,detached;
        Frame(long buffer,long timestamp,int dataSpace){this.buffer=buffer;this.timestamp=timestamp;this.dataSpace=dataSpace;}
        void release(){if(planesLocked){planesLocked=false;HostHardwareBuffer.unlockImagePlane(buffer);}HostNativeHandle.release(buffer);}
    }
    private static final class Reader implements AutoCloseable {
        final int width,height,format,dataSpace,maxImages;final long usage;final Object weakSelf;
        final ArrayDeque<Frame> pending=new ArrayDeque<>();
        final IdentityHashMap<Image,Frame> acquired=new IdentityHashMap<>();
        long surface;boolean transferred,closed,producerClosed;
        Reader(Object weakSelf,int width,int height,int maxImages,long usage,int format,int dataSpace){
            if(maxImages<1)throw new IllegalArgumentException("ImageReader maxImages must be positive");
            this.weakSelf=weakSelf;this.width=width;this.height=height;this.maxImages=maxImages;this.usage=usage;this.format=format;this.dataSpace=dataSpace;
            surface=HostSurface.create(width,height,format,new HostSurface.Target(){
                // This window receives the shared RGBA Canvas/video staging
                // producer. The queued allocation retains the reader's format.
                public long createNativeSurface(int w,int h,int f){return maclauncher.android.bridge.NativeSurface.create(w,h,android.graphics.PixelFormat.RGBA_8888);}
                public boolean submit(int[] pixels,int w,int h,long frame,long timestamp){return Reader.this.submit(pixels,w,h,frame,timestamp);}
                public int queueBuffer(long buffer,long frame,long timestamp,int space){return Reader.this.queueBuffer(buffer,timestamp,space);}
                public void resize(int w,int h){}
                public void close(){synchronized(Reader.this){producerClosed=true;}}
            });
        }
        public boolean submit(int[] pixels,int width,int height,long frameId,long timestamp){
            long buffer;
            synchronized(this){
                if(closed||producerClosed)return false;
                buffer=HostHardwareBuffer.allocate(width,height,format,usage);
                if(buffer==0)throw new UnsupportedOperationException("ImageReader hardware buffer configuration is unsupported");
                try{HostHardwareBuffer.writePixels(buffer,pixels);}
                catch(Throwable error){HostNativeHandle.release(buffer);throw error;}
            }
            int space=format==35||format==54||format==60?android.hardware.DataSpace.DATASPACE_JFIF:android.hardware.DataSpace.DATASPACE_SRGB;
            return publish(new Frame(buffer,timestamp,space));
        }
        int queueBuffer(long buffer,long timestamp,int dataSpace){
            if(format!=android.graphics.ImageFormat.PRIVATE&&!HostHardwareBuffer.compatibleFormat(format,HostHardwareBuffer.nGetFormat(buffer)))return -22;
            return publish(new Frame(HostNativeHandle.retain(buffer),timestamp,dataSpace))?0:-19;
        }
        boolean publish(Frame frame){
            Frame replaced;
            synchronized(this){
                if(closed||producerClosed){frame.release();return false;}
                // AOSP's app-controlled producer and BufferItemConsumer use
                // nonblocking dequeue and replace the last droppable frame.
                replaced=pending.pollLast();pending.addLast(frame);
            }
            if(replaced!=null)replaced.release();
            // No consumer lock is held while the original callback inspects its
            // listener, validity and Executor. The APK keeps callback ownership.
            try{postEvent.invokeExact(weakSelf);}catch(Throwable error){throw failure(error);}
            return true;
        }
        @Override public void close(){
            long untransferred;List<Frame> queued;
            synchronized(this){if(closed)return;closed=true;queued=new ArrayList<>(pending);pending.clear();untransferred=transferred?0:surface;transferred=true;}
            for(Frame frame:queued)frame.release();
            // The original Java Surface owns a transferred producer reference.
            // Closing the consumer never frees a producer still held elsewhere.
            if(untransferred!=0)HostNativeHandle.release(untransferred);
        }
    }
    private static Reader reader(ImageReader value){long handle=(long)context.get(value);if(handle==0)throw new IllegalStateException("ImageReader has closed");return (Reader)HostNativeHandle.state(handle);}
    private static long buffer(Object image){long handle=(long)imageBuffer.get(image);if(handle==0)throw new IllegalStateException("Image is not initialized");return handle;}
    public static void nativeClassInit(){Objects.requireNonNull(surfacePlaneConstructor);}
    public static void nativeInit(ImageReader value,Object weakSelf,int width,int height,int maxImages,long usage,int format,int dataSpace){
        if((long)context.get(value)!=0)throw new IllegalStateException("ImageReader is already initialized");
        var owner=new Reader(weakSelf,width,height,maxImages,usage,format,dataSpace);long handle=0;
        try{handle=HostNativeHandle.create(owner);context.set(value,handle);}catch(Throwable error){if(handle!=0)HostNativeHandle.release(handle);else owner.close();throw error;}
    }
    public static Surface nativeGetSurface(ImageReader value){
        Reader owner=reader(value);long handle;
        synchronized(owner){if(owner.closed)throw new IllegalStateException("ImageReader has closed");handle=owner.transferred?HostNativeHandle.retain(owner.surface):owner.surface;owner.transferred=true;}
        try{var surface=new Surface();HostSurface.assign(surface,handle);return surface;}catch(Throwable error){HostNativeHandle.release(handle);throw error;}
    }
    public static int nativeImageSetup(ImageReader value,Image image){
        Reader owner=reader(value);synchronized(owner){
            if(owner.closed)return 1;
            if(owner.acquired.size()>=owner.maxImages)return 2;
            Frame frame=owner.pending.pollFirst();if(frame==null)return 1;
            owner.acquired.put(image,frame);imageBuffer.set(image,frame.buffer);imageTime.set(image,frame.timestamp);
            imageDataSpace.set(image,frame.dataSpace);imageTransform.set(image,0);imageScaling.set(image,0);return 0;
        }
    }
    public static void nativeReleaseImage(ImageReader value,Image image){
        Reader owner=reader(value);Frame frame;
        synchronized(owner){frame=owner.acquired.remove(image);if(frame==null)throw new IllegalArgumentException("Image was not acquired from this ImageReader");imageBuffer.set(image,0L);}
        frame.release();
    }
    public static int nativeDetachImage(ImageReader value,Image image,boolean throwISEOnly){
        Reader owner=reader(value);synchronized(owner){Frame frame=owner.acquired.get(image);if(frame==null||frame.detached)throw new IllegalStateException("Image is not attached to this ImageReader");
            if(frame.planesLocked){frame.planesLocked=false;HostHardwareBuffer.unlockImagePlane(frame.buffer);}frame.detached=true;return 0;}
    }
    public static void nativeDiscardFreeBuffers(ImageReader value){reader(value);/* Released buffers are retired immediately by their existing owner. */}
    public static void nativeClose(ImageReader value){long handle=(long)context.getAndSet(value,0L);if(handle!=0)HostNativeHandle.release(handle);}
    public static int nativeGetWidth(Object image){return HostHardwareBuffer.nGetWidth(buffer(image));}
    public static int nativeGetHeight(Object image){return HostHardwareBuffer.nGetHeight(buffer(image));}
    public static int nativeGetFormat(Object image,int readerFormat){
        long handle=buffer(image);if(readerFormat==android.graphics.ImageFormat.PRIVATE)return readerFormat;
        try{Reader owner=reader((ImageReader)imageReader.invoke(image));int actual=HostHardwareBuffer.nGetFormat(handle);
            if(owner.format==android.graphics.ImageFormat.PRIVATE)return android.graphics.ImageFormat.PRIVATE;
            if(actual==android.graphics.ImageFormat.NV21||actual==android.graphics.ImageFormat.YV12)actual=android.graphics.ImageFormat.YUV_420_888;
            return HostHardwareBuffer.publicFormat(actual,(int)imageDataSpace.get(image));
        }catch(Throwable error){throw failure(error);}
    }
    public static int nativeGetFenceFd(Object image){buffer(image);return -1;/* The shared CPU producer completes writes before publication. */}
    public static HardwareBuffer nativeGetHardwareBuffer(Object image){return HostHardwareBuffer.wrap(HostNativeHandle.retain(buffer(image)));}
    public static Object[] nativeCreatePlanes(Object image,int numPlanes,int format,long usage){
        long handle=buffer(image);var planes=(Object[])Array.newInstance(surfacePlane,numPlanes);
        if(format==android.graphics.ImageFormat.PRIVATE){if(numPlanes!=0)throw new IllegalArgumentException("Opaque images have no accessible planes");return planes;}
        if(!HostHardwareBuffer.compatibleFormat(format,HostHardwareBuffer.nGetFormat(handle)))throw new UnsupportedOperationException("Image plane configuration is unsupported");
        ImageReader value;
        try{value=(ImageReader)imageReader.invoke(image);}catch(Throwable error){throw failure(error);}
        Reader owner=reader(value);synchronized(owner){Frame frame=owner.acquired.get(image);if(frame==null)throw new IllegalStateException("Image is not acquired");if(frame.planesLocked)throw new IllegalStateException("Image planes are already locked");
            HostHardwareBuffer.Plane[] storage=HostHardwareBuffer.lockImagePlanes(handle,numPlanes);
            try{for(int i=0;i<numPlanes;i++)planes[i]=surfacePlaneConstructor.invoke(image,storage[i].rowStride(),storage[i].pixelStride(),storage[i].bytes());frame.planesLocked=true;return planes;}
            catch(Throwable error){HostHardwareBuffer.unlockImagePlane(handle);throw failure(error);}}
    }
    public static ImageReader.ImagePlane[] nativeCreateImagePlanes(int numPlanes,GraphicBuffer graphic,int fenceFd,int format,int left,int top,int right,int bottom){
        if(format==android.graphics.ImageFormat.PRIVATE){if(numPlanes!=0)throw new IllegalArgumentException("Opaque images have no accessible planes");return new ImageReader.ImagePlane[0];}
        long handle=HostHardwareBuffer.graphicHandle(Objects.requireNonNull(graphic));
        if(numPlanes<0)throw new NegativeArraySizeException(Integer.toString(numPlanes));
        if(!HostHardwareBuffer.compatibleFormat(HostHardwareBuffer.hardwareFormat(format),HostHardwareBuffer.nGetFormat(handle)))throw new UnsupportedOperationException("Image plane configuration is unsupported");
        int width=HostHardwareBuffer.nGetWidth(handle),height=HostHardwareBuffer.nGetHeight(handle);
        if(left<0||top<0||right<left||bottom<top||right>width||bottom>height)throw new IllegalArgumentException("Image crop lies outside its GraphicBuffer");
        if(fenceFd>=0)try(var fence=SyncFence.adopt(fenceFd)){if(!fence.awaitForever())throw new IllegalStateException("Graphic buffer fence wait failed");}
        HostHardwareBuffer.Plane[] storage=HostHardwareBuffer.lockImagePlanes(handle,numPlanes);
        try{var planes=new ImageReader.ImagePlane[numPlanes];for(int i=0;i<numPlanes;i++)planes[i]=(ImageReader.ImagePlane)imagePlaneConstructor.invoke(storage[i].rowStride(),storage[i].pixelStride(),storage[i].bytes());return planes;}
        catch(Throwable error){HostHardwareBuffer.unlockImagePlane(handle);throw failure(error);}
    }
    public static void nativeUnlockGraphicBuffer(GraphicBuffer graphic){HostHardwareBuffer.unlockImagePlane(HostHardwareBuffer.graphicHandle(Objects.requireNonNull(graphic)));}
}
