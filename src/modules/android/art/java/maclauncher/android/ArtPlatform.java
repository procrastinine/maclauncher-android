package maclauncher.android;
import android.os.IBinder;
import android.view.SurfaceControl;
/** VM-specific JNI representation; resource policy stays with shared owners. */
final class ArtPlatform {
 private ArtPlatform(){}
 static native void initializeRpc(int hostUid);
 static native void libraryCompatibility(int targetSdk,boolean pageSizeCompatibility);
 static native SurfaceControl output();
 static native void orientation(boolean landscape);
 static native void activity(IBinder manager);
 static native void service(String name,IBinder manager);
 static native void audioService(IBinder manager);
 static native void audio(int[] ranges);
 static native void network(int netId,int state);
 static native IBinder connectivity();
 static native android.view.KeyCharacterMap keyboard(String contents);
}
