package maclauncher.android.bridge;

import android.app.Activity;

/** UI producer for one existing native surface and its existing presenter. */
public final class CanvasOverlay {
    public static CanvasFrameSource hostCreate(Activity activity,long handle,int width,int height){
        return new CanvasFrameSource(activity,new CanvasFrameSource.Backend(){
            @Override public void resize(int w,int h){validateNative(handle,w,h);}
            @Override public boolean submit(int[] pixels,int w,int h,long frame,long nanos){
                int result=submitNative(handle,pixels,w,h,frame);
                if(result==0)return true;if(result==3)return false;
                throw new IllegalStateException("Android overlay upload did not accept the image");
            }
        },width,height);
    }
    private static native int submitNative(long handle,int[] pixels,int width,int height,long frameId);
    private static native void validateNative(long handle,int width,int height);
    private CanvasOverlay(){}
}
