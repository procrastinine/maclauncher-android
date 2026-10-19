package maclauncher.android.framework;

import android.util.SparseArray;
import java.io.*;
import java.lang.ref.WeakReference;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;

/** Native inotify platform boundary; the original ObserverThread owns thread and observer dispatch. */
public final class HostFileObserver {
    private HostFileObserver() {}
    private static final Object LOCK=new Object();
    private static Path root;private static Thread worker;private static boolean loaded,closed;
    public static void configure(String directory){try{Path path=Path.of(directory).toRealPath();synchronized(LOCK){if(closed)throw new IllegalStateException("File observation is closed");if(root!=null&&!root.equals(path))throw new IllegalStateException("File observation is already configured");root=path;load();nativeInstall();}}catch(IOException error){throw new UncheckedIOException(error);}}
    public static int init(Object owner){synchronized(LOCK){if(closed)return -1;if(worker!=null)throw new IllegalStateException("File observation already has an owner");worker=(Thread)owner;worker.setDaemon(true);load();return nativeInit();}}
    public static void startWatching(Object owner,int descriptor,String[] paths,int mask,int[] output){synchronized(LOCK){if(closed||descriptor<0)return;if(root==null)throw new IllegalStateException("Application file observation is not configured");if(paths.length!=output.length)throw new IllegalArgumentException("Invalid file watch output");
        try{for(int i=0;i<paths.length;i++){Path actual=HostFilesystem.file(new File(paths[i])).toPath();int token=nativeStart(root.toString().getBytes(StandardCharsets.UTF_8),actual.toAbsolutePath().toString().getBytes(StandardCharsets.UTF_8),mask);output[i]=token;}}catch(RuntimeException|Error failure){for(int i=0;i<output.length;i++){if(output[i]>=0)nativeStop(output[i]);output[i]=-1;}throw failure;}
    }}
    private static SparseArray<WeakReference<?>> observers(Object owner){try{Field field=owner.getClass().getDeclaredField("mRealObservers");field.setAccessible(true);@SuppressWarnings("unchecked") var values=(SparseArray<WeakReference<?>>)field.get(owner);return values;}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}}
    public static void stopWatching(Object owner,int descriptor,int[] watches){synchronized(LOCK){if(!loaded||closed)return;var values=observers(owner);synchronized(values){for(int watch:watches){if(watch>=0)nativeStop(watch);values.remove(watch);}}}}
    public static void observe(Object owner,int descriptor){if(descriptor<0)return;int[] event=new int[2];long nextRetire=0;
        try{Method dispatch=owner.getClass().getDeclaredMethod("onEvent",int.class,int.class,String.class);dispatch.setAccessible(true);
            while(true){byte[] path=nativeNext(event);synchronized(LOCK){if(closed)return;long now=System.nanoTime();if(now>=nextRetire){nextRetire=now+1_000_000_000L;var values=observers(owner);synchronized(values){for(int i=values.size()-1;i>=0;i--){WeakReference<?> reference=values.valueAt(i);if(reference==null||reference.get()==null){nativeStop(values.keyAt(i));values.removeAt(i);}}}}}
                if(event[1]!=0)dispatch.invoke(owner,event[0],event[1],path==null?null:new String(path,StandardCharsets.UTF_8));
            }
        }catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}
        finally{synchronized(LOCK){closed=true;if(loaded)nativeClose();worker=null;}}
    }
    public static void shutdown(){Thread thread;synchronized(LOCK){closed=true;if(loaded)nativeClose();thread=worker;}if(thread!=null&&thread!=Thread.currentThread()){try{thread.join(1000);}catch(InterruptedException error){Thread.currentThread().interrupt();}if(thread.isAlive())throw new IllegalStateException("File observer callback did not finish during shutdown");}}
    private static void load(){if(!loaded){System.load(java.util.Objects.requireNonNull(System.getProperty("klepton.bridge")));loaded=true;}}
    private static native void nativeInstall();
    private static native int nativeInit();
    private static native int nativeStart(byte[] root,byte[] path,int mask);
    private static native void nativeStop(int token);
    private static native byte[] nativeNext(int[] event);
    private static native void nativeClose();
}
