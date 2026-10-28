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
    private static final MethodHandle hardwareConstructor,graphicConstructor,halFormat,publicFormat;
    static {try{
        var lookup=MethodHandles.lookup();var hardware=MethodHandles.privateLookupIn(HardwareBuffer.class,lookup);var graphic=MethodHandles.privateLookupIn(GraphicBuffer.class,lookup);
        hardwarePointer=hardware.findVarHandle(HardwareBuffer.class,"mNativeObject",long.class);graphicPointer=graphic.findVarHandle(GraphicBuffer.class,"mNativeObject",long.class);
        hardwareConstructor=hardware.findConstructor(HardwareBuffer.class,MethodType.methodType(void.class,long.class));
        graphicConstructor=graphic.findConstructor(GraphicBuffer.class,MethodType.methodType(void.class,int.class,int.class,int.class,int.class,long.class));
        Class<?> formatType=Class.forName("android.media.PublicFormatUtils",false,HardwareBuffer.class.getClassLoader());var formats=MethodHandles.privateLookupIn(formatType,lookup);
        halFormat=formats.findStatic(formatType,"getHalFormat",MethodType.methodType(int.class,int.class));
        publicFormat=formats.findStatic(formatType,"getPublicFormat",MethodType.methodType(int.class,int.class,int.class));
    }catch(ReflectiveOperationException failure){throw new ExceptionInInitializerError(failure);}}
    private HostHardwareBuffer(){}
    static final class Buffer implements AutoCloseable {
        final int width,height,format;final long usage;
        long storage;Bitmap bitmap;int[] pixels;Canvas locked;Thread thread;int imageLocks;
        Buffer(int width,int height,int format,long usage){this.width=width;this.height=height;this.format=format;this.usage=usage;storage=createStorage(width,height,format);}
        void live(){if(storage==0)throw new IllegalStateException("Hardware buffer has closed");}
        @Override public synchronized void close(){if(storage==0)return;long previous=storage;storage=0;try{if(locked!=null)locked.setBitmap(null);if(bitmap!=null)bitmap.recycle();}finally{locked=null;thread=null;bitmap=null;pixels=null;releaseStorage(previous);}}
    }
    static Buffer buffer(long handle){return (Buffer)HostNativeHandle.state(handle);}
    public static long allocate(int width,int height,int format,long usage){return nCreateHardwareBuffer(width,height,format,1,usage);}
    public static HardwareBuffer wrap(long ownedHandle){try{return (HardwareBuffer)hardwareConstructor.invokeExact(ownedHandle);}catch(RuntimeException|Error failure){HostNativeHandle.release(ownedHandle);throw failure;}catch(Throwable failure){HostNativeHandle.release(ownedHandle);throw new IllegalStateException(failure);}}
    public static long handle(HardwareBuffer buffer){long handle=(long)hardwarePointer.get(buffer);if(handle==0)throw new IllegalStateException("Hardware buffer has closed");return handle;}
    static long graphicHandle(GraphicBuffer buffer){long handle=(long)graphicPointer.get(buffer);if(handle==0)throw new IllegalStateException("Graphic buffer has closed");return handle;}
    record Plane(ByteBuffer bytes,int rowStride,int pixelStride){}
    static Plane[] lockImagePlanes(long handle,int count){
        Buffer value=buffer(handle);synchronized(value){value.live();if(value.locked!=null)throw new IllegalStateException("Graphic buffer Canvas is locked");
            if(count<0)throw new NegativeArraySizeException(Integer.toString(count));
            if(count>storagePlaneCount(value.storage))throw new IllegalArgumentException("Image has fewer planes than requested");
            if(count>0&&(value.format==35&&((value.width|value.height)&1)!=0||(value.format==54||value.format==60)&&(value.height&1)!=0))
                throw new UnsupportedOperationException("YUV image dimensions do not satisfy the original plane layout");
            HostNativeHandle.retain(handle);try{var planes=new Plane[count];for(int i=0;i<count;i++){
                int[] layout=storagePlaneLayout(value.storage,i);ByteBuffer bytes=storagePlane(value.storage,i);
                if(value.format==33){
                    layout[0]=0;layout[1]=0;int size=value.width;
                    if(size>=8){var footer=bytes.duplicate().order(java.nio.ByteOrder.nativeOrder());int id=footer.getInt(size-8),length=footer.getInt(size-4);
                        if((id==0xff||id==0xfe)&&length!=0){if(length<0||length>size)throw new UnsupportedOperationException("Encoded buffer footer exceeds its allocation");size=length;}}
                    bytes.limit(size);bytes=bytes.slice();
                }
                planes[i]=new Plane(bytes,layout[0],layout[1]);
            }value.imageLocks++;return planes;}catch(Throwable failure){HostNativeHandle.release(handle);throw failure;}}
    }
    static void unlockImagePlane(long handle){
        Buffer value=buffer(handle);synchronized(value){value.live();if(value.imageLocks==0)throw new IllegalStateException("Graphic buffer has no image lock");value.imageLocks--;}
        HostNativeHandle.release(handle);
    }
    /** Borrowed IOSurface/plane pointers remain valid while the caller retains the handle. */
    public static long storage(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return surfaceStorage(value.storage);}}
    public static ByteBuffer plane(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return storagePlane(value.storage,0);}}
    public static int rowStride(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return storageRowStride(value.storage);}}
    public static void writePixels(long handle,int[] pixels){Buffer value=buffer(handle);synchronized(value){value.live();if(value.imageLocks!=0)throw new IllegalStateException("Hardware buffer image planes are locked");if(value.format==32||value.format==33)throw new UnsupportedOperationException("Raw and encoded buffers require producer-supplied bytes");storagePixels(value.storage,pixels,true,value.format==HardwareBuffer.RGBX_8888);}}
    public static int[] readPixels(long handle){Buffer value=buffer(handle);synchronized(value){value.live();if(value.format==32||value.format==33)throw new UnsupportedOperationException("Raw and encoded buffers have no ARGB pixel view");int[] result=new int[Math.multiplyExact(value.width,value.height)];storagePixels(value.storage,result,false,value.format==HardwareBuffer.RGBX_8888);return result;}}
    public static long nativeBuffer(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return value.storage;}}
    public static int present(long handle,long surfaceLease){Buffer value=buffer(handle);synchronized(value){value.live();return presentStorage(value.storage,surfaceLease);}}
    public static long nCreateHardwareBuffer(int width,int height,int format,int layers,long usage){
        if(!nIsSupported(width,height,format,layers,usage))return 0;
        Buffer value=new Buffer(width,height,format,usage);try{return HostNativeHandle.create(value);}catch(Throwable failure){value.close();throw failure;}
    }
    public static boolean nIsSupported(int width,int height,int format,int layers,long usage){
        return width>0&&height>0&&(long)width*height<=Integer.MAX_VALUE/8&&layers==1&&supportsStorage(format)&&(format!=33||height==1)
            &&(usage&(HardwareBuffer.USAGE_PROTECTED_CONTENT|HardwareBuffer.USAGE_GPU_CUBE_MAP|HardwareBuffer.USAGE_GPU_MIPMAP_COMPLETE|HardwareBuffer.USAGE_SENSOR_DIRECT_DATA))==0;
    }
    public static long nGetNativeFinalizer(){return HostNativeHandle.finalizer();}
    public static int nGetWidth(long handle){return buffer(handle).width;}
    public static int nGetHeight(long handle){return buffer(handle).height;}
    public static int nGetFormat(long handle){return buffer(handle).format;}
    public static int nGetLayers(long handle){buffer(handle);return 1;}
    public static long nGetUsage(long handle){return buffer(handle).usage;}
    public static long nGetId(long handle){return storage(handle);}
    public static long nEstimateSize(long handle){Buffer value=buffer(handle);synchronized(value){value.live();return storageSize(value.storage);}}
    static boolean compatibleFormat(int requested,int actual){return requested==actual||requested==android.graphics.ImageFormat.YUV_420_888&&(actual==android.graphics.ImageFormat.NV21||actual==android.graphics.ImageFormat.YV12);}
    static int hardwareFormat(int format){try{return (int)halFormat.invokeExact(format);}catch(RuntimeException|Error error){throw error;}catch(Throwable error){throw new IllegalStateException(error);}}
    static int publicFormat(int format,int dataSpace){try{return (int)publicFormat.invokeExact(format,dataSpace);}catch(RuntimeException|Error error){throw error;}catch(Throwable error){throw new IllegalStateException(error);}}
    /*
     * Copyright (C) 2019 The Android Open Source Project
     *
     * Licensed under the Apache License, Version 2.0 (the "License");
     * you may not use this file except in compliance with the License.
     * You may obtain a copy of the License at
     *
     *      http://www.apache.org/licenses/LICENSE-2.0
     *
     * Unless required by applicable law or agreed to in writing, software
     * distributed under the License is distributed on an "AS IS" BASIS,
     * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
     * See the License for the specific language governing permissions and
     * limitations under the License.
     */
    // Complete device mappings from AOSP frameworks/native PublicFormat.cpp,
    // 2827a4a16b0340ecd07c2d5a6c89991799b362bb (Apache-2.0):
    // https://android.googlesource.com/platform/frameworks/native/+/2827a4a16b0340ecd07c2d5a6c89991799b362bb/libs/ui/PublicFormat.cpp
    // The supplied host utility uses identity conversions. Keep the original
    // PublicFormatUtils Java wrappers and replace only these native operations.
    public static int nativeGetHalFormat(int format){return switch(format){
        case ImageFormat.JPEG,ImageFormat.DEPTH_POINT_CLOUD,ImageFormat.DEPTH_JPEG,ImageFormat.HEIC,0x1006,ImageFormat.JPEG_R->HardwareBuffer.BLOB;
        case ImageFormat.DEPTH16->0x20363159;
        case ImageFormat.RAW_SENSOR,0x1002->ImageFormat.RAW_SENSOR;
        case 0x1003->ImageFormat.RAW10;
        default->format;
    };}
    public static int nativeGetHalDataspace(int format){return switch(format){
        case ImageFormat.JPEG,ImageFormat.YUV_420_888,ImageFormat.NV21,ImageFormat.YV12->android.hardware.DataSpace.DATASPACE_JFIF;
        case ImageFormat.DEPTH_POINT_CLOUD,ImageFormat.DEPTH16,0x1002,0x1003->android.hardware.DataSpace.DATASPACE_DEPTH;
        case ImageFormat.RAW_SENSOR,ImageFormat.RAW_PRIVATE,ImageFormat.RAW10,ImageFormat.RAW12->1; // HAL_DATASPACE_ARBITRARY
        case ImageFormat.DEPTH_JPEG->android.hardware.DataSpace.DATASPACE_DYNAMIC_DEPTH;
        case ImageFormat.HEIC->android.hardware.DataSpace.DATASPACE_HEIF;
        case 0x1006->android.hardware.DataSpace.DATASPACE_HEIF_ULTRAHDR;
        case ImageFormat.JPEG_R->android.hardware.DataSpace.DATASPACE_JPEG_R;
        default->android.hardware.DataSpace.DATASPACE_UNKNOWN;
    };}
    public static int nativeGetPublicFormat(int format,int dataSpace){return switch(format){
        case ImageFormat.RAW10->dataSpace==android.hardware.DataSpace.DATASPACE_DEPTH?0x1003:ImageFormat.RAW10;
        case ImageFormat.RAW_SENSOR->dataSpace==android.hardware.DataSpace.DATASPACE_DEPTH?0x1002:ImageFormat.RAW_SENSOR;
        case 0x20363159->dataSpace==android.hardware.DataSpace.DATASPACE_DEPTH?ImageFormat.DEPTH16:0x20363159;
        case HardwareBuffer.BLOB->switch(dataSpace){
            case android.hardware.DataSpace.DATASPACE_DEPTH->ImageFormat.DEPTH_POINT_CLOUD;
            case android.hardware.DataSpace.DATASPACE_HEIF->ImageFormat.HEIC;
            case android.hardware.DataSpace.DATASPACE_DYNAMIC_DEPTH->ImageFormat.DEPTH_JPEG;
            case android.hardware.DataSpace.DATASPACE_JPEG_R->ImageFormat.JPEG_R;
            case android.hardware.DataSpace.DATASPACE_HEIF_ULTRAHDR->0x1006;
            default->ImageFormat.JPEG;
        };
        case PixelFormat.RGBA_8888,PixelFormat.RGBX_8888,PixelFormat.RGB_888,PixelFormat.RGB_565,
             HardwareBuffer.RGBA_FP16,HardwareBuffer.RGBA_1010102,ImageFormat.Y8,ImageFormat.RAW12,
             ImageFormat.YUV_420_888,ImageFormat.YV12,HardwareBuffer.YCBCR_P010,
             ImageFormat.RAW_PRIVATE,ImageFormat.NV16,ImageFormat.NV21,ImageFormat.YUY2,ImageFormat.PRIVATE->format;
        // P210 is declared by this same pinned PublicFormat enum and selected
        // Java profile, retaining the complete profile's 4:2:2 10-bit identity.
        case 60->60;
        default->ImageFormat.UNKNOWN;
    };}
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
        Buffer value=buffer(handle);synchronized(value){value.live();if(value.locked!=null||value.imageLocks!=0||value.format==32||value.format==33)return false;
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
    private static native boolean supportsStorage(int format);
    private static native long createStorage(int width,int height,int format);
    private static native void releaseStorage(long storage);
    private static native long surfaceStorage(long storage);
    private static native int storagePlaneCount(long storage);
    private static native int[] storagePlaneLayout(long storage,int index);
    private static native ByteBuffer storagePlane(long storage,int index);
    private static native long storageSize(long storage);
    private static native int storageRowStride(long storage);
    private static native void storagePixels(long storage,int[] pixels,boolean write,boolean opaque);
    private static native int presentStorage(long storage,long surfaceLease);
}
