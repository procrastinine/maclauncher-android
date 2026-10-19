package maclauncher.android;

import android.os.Looper;
import android.os.MessageQueue;
import android.os.ParcelFileDescriptor;
import java.io.IOException;

/** ART callback references stay on the original Android window queue. */
final class ArtWebKit implements AutoCloseable {
    private final MessageQueue queue;
    private final ParcelFileDescriptor descriptor;
    private boolean closed;
    ArtWebKit(String socket,String capability){
        if(Looper.myLooper()!=Looper.getMainLooper())throw new IllegalStateException("Browser transport requires its Android window thread");
        queue=Looper.myQueue();descriptor=ParcelFileDescriptor.adoptFd(openNative(socket,capability));
        try{queue.addOnFileDescriptorEventListener(descriptor.getFileDescriptor(),MessageQueue.OnFileDescriptorEventListener.EVENT_INPUT,(fd,events)->{
            dispatchNative();return closed?0:MessageQueue.OnFileDescriptorEventListener.EVENT_INPUT;
        });}catch(RuntimeException|Error failure){try{close();}catch(IOException cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }
    @Override public void close()throws IOException {
        if(closed)return;closed=true;
        queue.removeOnFileDescriptorEventListener(descriptor.getFileDescriptor());
        try{closeNative();}finally{descriptor.close();}
    }
    private static native int openNative(String socket,String capability);
    private static native void dispatchNative();
    private static native void closeNative();
}
