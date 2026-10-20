import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class EglPixelCopyProbe {
    private final CountDownLatch done=new CountDownLatch(1);
    private int status=-1;
    private int[] pixels;
    public void complete(int code,int[] data){status=code;pixels=data;done.countDown();}
    public void await(int color)throws Exception{
        if(!done.await(2,TimeUnit.SECONDS))throw new AssertionError("Idle EGL PixelCopy did not complete");
        if(status!=0||pixels==null||pixels.length!=4)throw new AssertionError("Idle EGL PixelCopy status "+status);
        for(int pixel:pixels)if(pixel!=color)throw new AssertionError("Submitted EGL image changed: "+Integer.toHexString(pixel)+" != "+Integer.toHexString(color));
    }
}
