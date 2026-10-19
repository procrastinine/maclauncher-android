package maclauncher.android.media;
import java.io.FileDescriptor;
import java.io.IOException;
public final class AudioHost {
    private static final java.lang.ref.Cleaner cleaner=java.lang.ref.Cleaner.create();
    static {maclauncher.android.framework.HostFrameworkLibraries.loadAudio();}
    public static void initialize() {}
    public static boolean streamActive(int stream,int inPastMs){return HostAudioManager.streamActive(stream,inPastMs);}
    public static native long load(String path, FileDescriptor fd, long offset, long size) throws IOException;
    public static native long loadMedia(String path, FileDescriptor fd, long offset, long size) throws IOException;
    public static native boolean action(long id, int action);
    public static native long copy(long id);
    public static native void setStream(long id, int stream);
    public static native void configure(long id, float left, float right, int loops, float rate);
    public static native double[] decodeStats(String path, FileDescriptor fd, long offset, long size) throws IOException;
    public static native int time(long id,boolean position);
    public static native void seek(long id,long milliseconds);
    /** Owns player handles without retaining the Java player or pool. */
    public static final class Ownership implements Runnable {
        private final java.util.Set<Long> handles=new java.util.HashSet<>();
        private final java.util.Set<android.os.ParcelFileDescriptor> sources=new java.util.HashSet<>();
        public synchronized long add(long value){if(value==0)throw new IllegalStateException("Audio returned no player");handles.add(value);return value;}
        public synchronized android.os.ParcelFileDescriptor add(android.os.ParcelFileDescriptor source){sources.add(source);return source;}
        public void release(long value){boolean owned;synchronized(this){owned=handles.remove(value);}if(owned)action(value,3);}
        public void release(android.os.ParcelFileDescriptor source){boolean owned;synchronized(this){owned=sources.remove(source);}if(owned)try{source.close();}catch(IOException ignored){}}
        public void run(){java.util.Set<Long> players;java.util.Set<android.os.ParcelFileDescriptor> descriptors;synchronized(this){players=java.util.Set.copyOf(handles);descriptors=java.util.Set.copyOf(sources);handles.clear();sources.clear();}for(var source:descriptors)try{source.close();}catch(IOException ignored){}for(long value:players)action(value,3);}
    }
    public static java.lang.ref.Cleaner.Cleanable track(Object owner,Ownership handles){return cleaner.register(owner,handles);}
}
