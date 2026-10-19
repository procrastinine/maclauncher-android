package maclauncher.android.framework;

import android.graphics.*;
import android.os.*;
import android.view.*;
import maclauncher.android.bridge.NativeSurface;

/** Native copy boundary; PixelCopy owns every public overload and callback adapter. */
public final class HostPixelCopy {
    private HostPixelCopy(){}
    public static void nCopySurfaceInto(Surface surface,int left,int top,int right,int bottom,HardwareRenderer.CopyRequest request){
        int[] geometry;
        try{geometry=NativeSurface.leaseGeometry(HostSurface.nativeLease(surface));}
        catch(IllegalStateException|IllegalArgumentException unavailable){request.onCopyFinished(PixelCopy.ERROR_SOURCE_INVALID);return;}
        Rect region=new Rect(left,top,right,bottom);if(region.isEmpty())region.set(0,0,geometry[0],geometry[1]);
        if(!region.intersect(0,0,geometry[0],geometry[1])){request.onCopyFinished(PixelCopy.ERROR_SOURCE_INVALID);return;}
        request.getDestinationBitmap(region.width(),region.height());
        Bitmap destination=(Bitmap)HostUiAccess.get(request,HardwareRenderer.CopyRequest.class,"mDestinationBitmap");
        if(destination==null||destination.isRecycled()||!destination.isMutable()){request.onCopyFinished(PixelCopy.ERROR_DESTINATION_INVALID);return;}
        int result=HostSurface.copyCanvasInto(surface,region,destination);
        if(result>=0){request.onCopyFinished(result);return;}
        Copy copy=new Copy(destination,request,Looper.getMainLooper());
        if(!HostLooper.addShutdownListener(copy.looper,copy)){request.onCopyFinished(PixelCopy.ERROR_SOURCE_INVALID);return;}
        try{long handle=NativeSurface.requestPixelCopyRegion(surface,region.left,region.top,region.right,region.bottom,destination.getWidth(),destination.getHeight(),copy);synchronized(copy){if(copy.closed){if(handle!=0)NativeSurface.cancelPixelCopy(handle);}else copy.handle=handle;}}
        catch(Throwable failure){copy.run();throw failure;}
    }
    private static final class Copy implements NativeSurface.PixelCopyCallback,Runnable {
        final Looper looper;Bitmap destination;HardwareRenderer.CopyRequest request;long handle;boolean closed;
        Copy(Bitmap destination,HardwareRenderer.CopyRequest request,Looper looper){this.destination=destination;this.request=request;this.looper=looper;}
        @Override public void complete(int status,int[] pixels){
            Bitmap bitmap;HardwareRenderer.CopyRequest callback;
            synchronized(this){if(closed)return;closed=true;handle=0;bitmap=destination;callback=request;destination=null;request=null;}
            HostLooper.removeShutdownListener(looper,this);
            if(status==PixelCopy.SUCCESS){if(bitmap.isRecycled()||!bitmap.isMutable()||pixels==null||(long)bitmap.getWidth()*bitmap.getHeight()!=pixels.length)status=PixelCopy.ERROR_DESTINATION_INVALID;else bitmap.setPixels(pixels,0,bitmap.getWidth(),0,0,bitmap.getWidth(),bitmap.getHeight());}
            callback.onCopyFinished(status);
        }
        @Override public void run(){long current;synchronized(this){if(closed)return;closed=true;current=handle;handle=0;destination=null;request=null;}HostLooper.removeShutdownListener(looper,this);if(current!=0)NativeSurface.cancelPixelCopy(current);}
    }
}
