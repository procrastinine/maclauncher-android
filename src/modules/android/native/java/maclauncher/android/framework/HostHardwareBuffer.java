package maclauncher.android.framework;

import android.graphics.*;
import android.hardware.HardwareBuffer;
import android.os.Parcel;
import java.lang.invoke.*;
import java.nio.ByteBuffer;

/** Native memory ownership below original HardwareBuffer and GraphicBuffer. */
public final class HostHardwareBuffer {
    static {String library=System.getProperty("klepton.graphics");if(library==null)throw new IllegalStateException("Native graphics library is unavailable");System.load(library);}
    private static final VarHandle hardwarePointer,graphicPointer;
    private static final MethodHandle hardwareConstructor,graphicConstructor;
    static {try{
        var lookup=MethodHandles.lookup();var hardware=MethodHandles.privateLookupIn(HardwareBuffer.class,lookup);var graphic=MethodHandles.privateLookupIn(GraphicBuffer.class,lookup);
        hardwarePointer=hardware.findVarHandle(HardwareBuffer.class,"mNativeObject",long.class);graphicPointer=graphic.findVarHandle(GraphicBuffer.class,"mNativeObject",long.class);
        hardwareConstructor=hardware.findConstructor(HardwareBuffer.class,MethodType.methodType(void.class,long.class));
        graphicConstructor=graphic.findConstructor(GraphicBuffer.class,MethodType.methodType(void.class,int.class,int.class,int.class,int.class,long.class));
    }catch(ReflectiveOperationException failure){throw new ExceptionInInitializerError(failure);}}
    private HostHardwareBuffer(){}
    static final class Buffer implements AutoCloseable {
        final int width,height,format;final long usage;
        long storage;Bitmap bitmap;int[] pixels;Canvas locked;Thread thread;int imageLocks;
        Buffer(int width,int height,int format,long usage){this.width=width;this.height=height;this.format=format;this.usage=usage;storage=createStorage(width,height);}
        void live(){if(storage==0)throw new IllegalStateException("Hardware buffer has closed");}
        @Override public synchronized void close(){if(storage==0)return;long previous=storage;storage=0;try{if(locked!=null)locked.setBitmap(null);if(bitmap!=null)bitmap.recycle();}finally{locked=null;thread=null;bitmap=null;pixels=null;releaseStorage(previous);}}
    }
    static Buffer buffer(long handle){return (Buffer)HostNativeHandle.state(handle);}
    public static long allocate(int width,int height,int format,long usage){return nCreateHardwareBuffer(width,height,format,1,usage);}
    public static HardwareBuffer wrap(long ownedHandle){try{return (HardwareBuffer)hardwareConstructor.invokeExact(ownedHandle);}catch(RuntimeException|Error failure){HostNativeHandle.release(ownedHandle);throw failure;}catch(Throwable failure){HostNativeHandle.release(ownedHandle);throw new IllegalStateException(failure);}}
    public static long handle(HardwareBuffer buffer){long handle=(long)hardwarePointer.get(buffer);if(handle==0)throw new IllegalStateException("Hardware buffer has closed");return handle;}
    static long graphicHandle(GraphicBuffer buffer){long handle=(long)graphicPointer.get(buffer);if(handle==0)throw new IllegalStateException("Graphic buffer has closed");return handle;}
    static ByteBuffer lockImagePlane(long handle){
        Buffer value=buffer(handle);synchronized(value){value.live();if(value.locked!=null)throw new IllegalStateException("Graphic buffer Canvas is locked");
            HostNativeHandle.retain(handle);try{ByteBuffer plane=storagePlane(value.storage);value.imageLocks++;return plane;}catch(Throwable failure){HostNativeHandle.release(handle);throw failure;}}
    }
    static void unlockImagePlane(long handle){
        Buffer value=buffer(handle);synchronized(value){value.live();if(value.imageLocks==0)throw new IllegalStateException("Graphic buffer has no image lock");value.imageLocks--;}
        HostNativeHandle.release(handle);
    }
    /** Borrowed IOSurface/plane pointers remain valid while the caller retains the handle. */
    public static long storage(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return surfaceStorage(value.storage);}}
    public static ByteBuffer plane(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return storagePlane(value.storage);}}
    public static int rowStride(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return storageRowStride(value.storage);}}
    public static void writePixels(long handle,int[] pixels){Buffer value=buffer(handle);synchronized(value){value.live();storagePixels(value.storage,pixels,true,value.format==HardwareBuffer.RGBX_8888);}}
    public static int[] readPixels(long handle){Buffer value=buffer(handle);synchronized(value){value.live();int[] result=new int[Math.multiplyExact(value.width,value.height)];storagePixels(value.storage,result,false,value.format==HardwareBuffer.RGBX_8888);return result;}}
    public static long nativeBuffer(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return value.storage;}}
    public static int present(long handle,long surfaceLease){Buffer value=buffer(handle);synchronized(value){value.live();return presentStorage(value.storage,surfaceLease);}}
    public static long nCreateHardwareBuffer(int width,int height,int format,int layers,long usage){
        if(!nIsSupported(width,height,format,layers,usage))return 0;
        Buffer value=new Buffer(width,height,format,usage);try{return HostNativeHandle.create(value);}catch(Throwable failure){value.close();throw failure;}
    }
    public static boolean nIsSupported(int width,int height,int format,int layers,long usage){
        return width>0&&height>0&&(long)width*height<=Integer.MAX_VALUE/4&&layers==1&&(format==HardwareBuffer.RGBA_8888||format==HardwareBuffer.RGBX_8888)
            &&(usage&(HardwareBuffer.USAGE_PROTECTED_CONTENT|HardwareBuffer.USAGE_GPU_CUBE_MAP|HardwareBuffer.USAGE_GPU_MIPMAP_COMPLETE|HardwareBuffer.USAGE_SENSOR_DIRECT_DATA))==0;
    }
    public static long nGetNativeFinalizer(){return HostNativeHandle.finalizer();}
    public static int nGetWidth(long handle){return buffer(handle).width;}
    public static int nGetHeight(long handle){return buffer(handle).height;}
    public static int nGetFormat(long handle){return buffer(handle).format;}
    public static int nGetLayers(long handle){buffer(handle);return 1;}
    public static long nGetUsage(long handle){return buffer(handle).usage;}
    public static long nGetId(long handle){return storage(handle);}
    public static long nEstimateSize(long handle){Buffer value=buffer(handle);return (long)rowStride(handle)*value.height;}
    public static void nWriteHardwareBufferToParcel(long handle,Parcel parcel){HostObjectParcel.writeNativeHandle(parcel,handle);}
    public static long nReadHardwareBufferFromParcel(Parcel parcel){return HostObjectParcel.readNativeHandle(parcel,Buffer.class);}
    public static long nCreateFromGraphicBuffer(GraphicBuffer value){long handle=(long)graphicPointer.get(value);buffer(handle);return HostNativeHandle.retain(handle);}
    public static long nCreateGraphicBuffer(int width,int height,int format,int usage){return nCreateHardwareBuffer(width,height,format,1,Integer.toUnsignedLong(usage));}
    public static void nDestroyGraphicBuffer(long handle){HostNativeHandle.release(handle);}
    public static void nWriteGraphicBufferToParcel(long handle,Parcel parcel){HostObjectParcel.writeNativeHandle(parcel,handle);}
    public static long nReadGraphicBufferFromParcel(Parcel parcel){return HostObjectParcel.readNativeHandle(parcel,Buffer.class);}
    public static GraphicBuffer nCreateFromHardwareBuffer(HardwareBuffer hardware){
        long handle=handle(hardware);Buffer value=buffer(handle);HostNativeHandle.retain(handle);
        try{return (GraphicBuffer)graphicConstructor.invokeExact(value.width,value.height,value.format,(int)value.usage,handle);}catch(RuntimeException|Error failure){HostNativeHandle.release(handle);throw failure;}catch(Throwable failure){HostNativeHandle.release(handle);throw new IllegalStateException(failure);}
    }
    public static boolean nLockCanvas(long handle,Canvas canvas,Rect dirty){
        Buffer value=buffer(handle);synchronized(value){value.live();if(value.locked!=null||value.imageLocks!=0)return false;
            if(value.bitmap==null){value.bitmap=Bitmap.createBitmap(value.width,value.height,Bitmap.Config.ARGB_8888);value.pixels=new int[Math.multiplyExact(value.width,value.height)];}
            storagePixels(value.storage,value.pixels,false,value.format==HardwareBuffer.RGBX_8888);value.bitmap.setPixels(value.pixels,0,value.width,0,0,value.width,value.height);
            canvas.setBitmap(value.bitmap);if(dirty!=null){if(!dirty.intersect(0,0,value.width,value.height))dirty.setEmpty();canvas.clipRect(dirty);}value.locked=canvas;value.thread=Thread.currentThread();return true;
        }
    }
    public static boolean nUnlockCanvasAndPost(long handle,Canvas canvas){
        Buffer value=buffer(handle);synchronized(value){value.live();if(value.locked!=canvas||value.thread!=Thread.currentThread())return false;
            try{value.bitmap.getPixels(value.pixels,0,value.width,0,0,value.width,value.height);storagePixels(value.storage,value.pixels,true,value.format==HardwareBuffer.RGBX_8888);return true;}finally{canvas.setBitmap(null);value.locked=null;value.thread=null;}
        }
    }
    private static native long createStorage(int width,int height);
    private static native void releaseStorage(long storage);
    private static native long surfaceStorage(long storage);
    private static native ByteBuffer storagePlane(long storage);
    private static native int storageRowStride(long storage);
    private static native void storagePixels(long storage,int[] pixels,boolean write,boolean opaque);
    private static native int presentStorage(long storage,long surfaceLease);
}
