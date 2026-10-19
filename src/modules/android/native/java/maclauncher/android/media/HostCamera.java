package maclauncher.android.media;

import android.hardware.Camera;
import android.graphics.SurfaceTexture;
import android.os.*;
import android.view.Surface;
import java.io.IOException;
import java.lang.reflect.*;
import java.util.*;

/** Camera-service boundary for explicitly selected AVFoundation sources, including OBS. */
public final class HostCamera {
    private HostCamera() {}
    private static final Method EVENT;
    static {try{EVENT=Camera.class.getDeclaredMethod("postEventFromNative",Object.class,int.class,int.class,int.class,Object.class);EVENT.setAccessible(true);}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    private static void event(Object ref,int type,int first,Object value){try{EVENT.invoke(null,ref,type,first,0,value);}catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}}
    private static final class Capture implements HostMediaObjects.State {
        long token;final Object reference;final Handler handler;final int[] sizes;
        int width,height;boolean running,callback,buffered;final ArrayDeque<byte[]> buffers=new ArrayDeque<>();
        Capture(long token,Object reference,int[] sizes){this.token=token;this.reference=reference;this.sizes=sizes;width=sizes[0];height=sizes[1];Looper loop=Looper.myLooper();handler=new Handler(loop==null?Objects.requireNonNull(Looper.getMainLooper()):loop);}
        private void check(){if(token==0)throw new IllegalStateException("Camera is released");}
        synchronized void start(){check();if(running)return;if(!MediaInputs.cameraStart(token,width,height))throw new IllegalStateException("Selected camera preview could not start");running=true;if(!handler.post(pump)){stop();throw new IllegalStateException("Camera callback Looper is closed");}}
        synchronized void stop(){running=false;handler.removeCallbacks(pump);if(token!=0)MediaInputs.cameraStop(token);buffers.clear();}
        public synchronized void close(){stop();if(token!=0){MediaInputs.cameraClose(token);token=0;}}
        private final Runnable pump=new Runnable(){public void run(){byte[] bytes=null;boolean failed=false;synchronized(Capture.this){
            if(!running||token==0)return;if(!MediaInputs.cameraSelected()){stop();return;}
            if(callback){bytes=buffered?buffers.poll():new byte[width*height*3/2];if(bytes!=null){int count=MediaInputs.cameraRead(token,bytes);if(count<0){stop();failed=true;bytes=null;}else if(count==0){if(buffered)buffers.addFirst(bytes);bytes=null;}else if(!buffered)callback=false;}}
            if(running&&!handler.postDelayed(this,34))stop();
        }if(failed)event(reference,1,Camera.CAMERA_ERROR_UNKNOWN,null);else if(bytes!=null)event(reference,16,0,bytes);}};
        synchronized String parameters(){check();StringJoiner list=new StringJoiner(",");for(int i=0;i+1<sizes.length;i+=2)list.add(sizes[i]+"x"+sizes[i+1]);return "preview-size="+width+"x"+height+";preview-size-values="+list+";preview-format=yuv420sp;preview-format-values=yuv420sp;focus-mode=fixed;focus-mode-values=fixed;preview-frame-rate=30;preview-frame-rate-values=30;preview-fps-range=30000,30000;preview-fps-range-values=(30000,30000);zoom-supported=false;video-stabilization-supported=false;max-num-detected-faces-hw=0;max-num-detected-faces-sw=0";}
        synchronized void parameters(String flat){check();if(flat==null||flat.length()>65536)throw new IllegalArgumentException("Invalid camera parameters");Map<String,String> values=new LinkedHashMap<>();for(String pair:flat.split(";")){int separator=pair.indexOf('=');if(separator>0)values.put(pair.substring(0,separator),pair.substring(separator+1));}
            String size=values.getOrDefault("preview-size",width+"x"+height);String[] dimensions=size.split("x",-1);if(dimensions.length!=2)throw new IllegalArgumentException("Invalid camera size");int w=Integer.parseInt(dimensions[0]),h=Integer.parseInt(dimensions[1]);boolean supported=false;for(int i=0;i+1<sizes.length;i+=2)if(sizes[i]==w&&sizes[i+1]==h)supported=true;
            if(!supported||!"yuv420sp".equals(values.getOrDefault("preview-format","yuv420sp"))||!"fixed".equals(values.getOrDefault("focus-mode","fixed")))throw new IllegalArgumentException("Unsupported camera capture format");
            if(running&&(w!=width||h!=height))throw new IllegalStateException("Stop preview before changing its size");width=w;height=h;
        }
    }
    private static Capture capture(Camera owner){return HostMediaObjects.get(owner,Capture.class);}
    public static int _getNumberOfCameras(Parcel attribution,int devicePolicy){return MediaInputs.cameraSelected()&&MediaInputs.cameraSizes().length>0?1:0;}
    public static void _getCameraInfo(int id,int rotation,Parcel attribution,int devicePolicy,Camera.CameraInfo info){Objects.requireNonNull(info);if(id!=0||_getNumberOfCameras(attribution,devicePolicy)==0)throw new IllegalArgumentException("Camera is unavailable");info.facing=Camera.CameraInfo.CAMERA_FACING_BACK;info.orientation=0;info.canDisableShutterSound=false;}
    public static int native_setup(Camera owner,Object reference,int id,int halVersion,boolean slowJpeg,Parcel attribution,int policy){if(id!=0||!MediaInputs.isGranted("android.permission.CAMERA"))return -1;int[] sizes=MediaInputs.cameraSizes();if(sizes.length<2)return -19;long token=MediaInputs.cameraOpen();if(token==0)return -16;Capture capture;try{capture=new Capture(token,reference,sizes);}catch(RuntimeException|Error failure){MediaInputs.cameraClose(token);throw failure;}HostMediaObjects.attach(owner,capture);return 0;}
    public static void native_release(Camera owner){HostMediaObjects.release(owner);}
    public static void startPreview(Camera owner){capture(owner).start();}
    public static void _stopPreview(Camera owner){capture(owner).stop();}
    public static boolean previewEnabled(Camera owner){Capture c=capture(owner);synchronized(c){return c.running;}}
    public static void setHasPreviewCallback(Camera owner,boolean present,boolean buffered){Capture c=capture(owner);synchronized(c){c.check();c.callback=present;c.buffered=buffered;if(!buffered)c.buffers.clear();}}
    public static void _addCallbackBuffer(Camera owner,byte[] data,int type){if(data==null)return;Capture c=capture(owner);synchronized(c){c.check();if(type!=16)throw new IllegalArgumentException("Only preview callback buffers are available");if(data.length<c.width*c.height*3/2)throw new IllegalArgumentException("Camera callback buffer is too small");if(c.buffers.size()>=16)throw new IllegalStateException("Camera callback buffer limit reached");for(byte[] pending:c.buffers)if(pending==data)throw new IllegalArgumentException("Camera callback buffer is already queued");c.buffers.add(data);}}
    public static void native_setParameters(Camera owner,String parameters){capture(owner).parameters(parameters);}
    public static String native_getParameters(Camera owner){return capture(owner).parameters();}
    public static boolean _enableShutterSound(Camera owner,boolean enabled){capture(owner);return !enabled;}
    public static void setPreviewSurface(Camera owner,Surface surface)throws IOException{capture(owner);if(surface!=null)throw new IOException("Camera surface preview is unavailable; preview callbacks are supported");}
    public static void setPreviewCallbackSurface(Camera owner,Surface surface){capture(owner);if(surface!=null)throw new IllegalArgumentException("Camera callback surfaces are unavailable");}
    public static void setPreviewTexture(Camera owner,SurfaceTexture texture)throws IOException{capture(owner);if(texture!=null)throw new IOException("Camera texture preview is unavailable; preview callbacks are supported");}
    public static void setDisplayOrientation(Camera owner,int degrees){capture(owner);if(degrees!=0&&degrees!=90&&degrees!=180&&degrees!=270)throw new IllegalArgumentException("Invalid camera orientation");/* No display preview is attached. Callback bytes retain the sensor orientation, as on Android. */}
}
