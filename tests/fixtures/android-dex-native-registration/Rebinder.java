package owned;
import maclauncher.android.bridge.GuestNativeLoader;
public final class Rebinder {
 public static void load(){GuestNativeLoader.loadLibrary("dexregistration");}
 public static native int unregister();
}
