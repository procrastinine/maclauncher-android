package maclauncher.android.media;


/** Explicit host-selected inputs. Configuration and enumeration never capture. */
public final class MediaInputs {
    private static boolean configured, closed;
    private static String audio="", camera="";
    static { String file=System.getProperty("klepton.audio");if(file==null)System.loadLibrary("klepton-audio");else System.load(file); }
    private MediaInputs(){}
    public static synchronized void configure(String audioUid,String cameraUid){
        if(configured||closed)throw new IllegalStateException("Media inputs already configured");
        audio=checked(audioUid);camera=checked(cameraUid);nativeConfigure(audio,camera);configured=true;
    }
    private static String checked(String value){
        if(value==null)return "";
        if(value.length()>512||value.getBytes(java.nio.charset.StandardCharsets.UTF_8).length>2048||value.chars().anyMatch(c->c<32||c==127))throw new IllegalArgumentException("Invalid media device identity");
        return value;
    }
    public static boolean isMediaPermission(String permission){return "android.permission.RECORD_AUDIO".equals(permission)||"android.permission.CAMERA".equals(permission);}
    public static synchronized boolean isGranted(String permission){
        if(!configured||closed)return false;
        return "android.permission.RECORD_AUDIO".equals(permission)?!audio.isEmpty()&&nativeAuthorization(false):
            "android.permission.CAMERA".equals(permission)&&!camera.isEmpty()&&nativeAuthorization(true);
    }
    public static synchronized void hostShutdown(){
        if(closed)return;closed=true;audio=camera="";
        nativeShutdown();
    }
    public static void main(String[] args){
        if(args.length!=1||!args[0].equals("--list"))throw new IllegalArgumentException("Use --list");
        System.out.println(nativeDevices());
    }
    public static synchronized boolean cameraSelected(){return configured&&!closed&&!camera.isEmpty();}
    private static native String nativeDevices();
    private static native void nativeConfigure(String audioUid,String cameraUid);
    private static native boolean nativeAuthorization(boolean camera);
    private static native void nativeShutdown();
    public static native long audioOpen(int rate,int channels);
    public static native boolean audioStart(long owner);
    public static native int audioRead(long owner,short[] data,int offset,int samples);
    public static native void audioStop(long owner);
    public static native void audioClose(long owner);
    public static native int[] cameraSizes();
    public static native long cameraOpen();
    public static native boolean cameraStart(long owner,int width,int height);
    public static native int cameraRead(long owner,byte[] nv21);
    public static native void cameraStop(long owner);
    public static native void cameraClose(long owner);
}
