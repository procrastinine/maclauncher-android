package maclauncher.android.bridge;
/** Private native owner. No window is created by class initialization. */
public final class WindowSession implements maclauncher.android.framework.HostActivity.Window {
    private static WindowSession current;
    private final Thread owner=Thread.currentThread();
    private long identity;
    private Runnable closeRequested;
    private WindowSession(long value){identity=value;}
    /** The process owns SDL/AppKit; individual windows borrow that owner. */
    public static native void configureIdentity(String title,byte[] png,boolean headless);
    public static native void shutdownApplication();
    public static WindowSession open(String title,int width,int height){
        return open(title,width,height,null);
    }
    public static WindowSession open(String title,int width,int height,Runnable closeRequested){
        if(title==null)throw new NullPointerException("title");
        if(width<1||height<1||width>8192||height>8192)throw new IllegalArgumentException("Invalid viewport");
        // The graphics library is already selected and loaded by Host.
        WindowSession result=new WindowSession(allocateNative());
        result.closeRequested=closeRequested;
        if(result.identity==0)throw new IllegalStateException("Native window session unavailable");
        try{initializeNative(result.identity,title,width,height);current=result;return result;}
        catch(Throwable primary){try{result.close();}catch(Throwable cleanup){if(cleanup!=primary)primary.addSuppressed(cleanup);}throw primary;}
    }
    @Override public void present(android.app.Activity activity,String title,int width,int height){
        maclauncher.android.framework.HostActivityManager.hostInitializeGraphics();
        NativeSurface.run(maclauncher.android.framework.HostActivity.content(activity),activity,title,width,height);
    }
    @Override public void close(){
        if(Thread.currentThread()!=owner)throw new IllegalStateException("Window session requires its owner thread");
        if(identity==0)return;
        if(!closeNative(identity))throw new IllegalStateException("Native window session still has an active producer or display callback");
        identity=0;closeRequested=null;
        if(current==this)current=null;
    }
    /** Bind the Activity to this existing window before its onCreate callback. */
    public void attachActivity(android.app.Activity activity){
        if(Thread.currentThread()!=owner||identity==0)throw new IllegalStateException("Window session requires its live owner thread");
        attachActivityNative(identity,java.util.Objects.requireNonNull(activity));
    }
    public static boolean attached(){return current!=null;}
    /** A native close gesture precedes guest callbacks, which may terminate the JVM. */
    public static void requestClose(){
        WindowSession session=current;
        if(session==null)return;
        if(Thread.currentThread()!=session.owner)throw new IllegalStateException("Window close requires its owner thread");
        Runnable notify=session.closeRequested;session.closeRequested=null;
        if(notify!=null)notify.run();
    }
    /** Switch the foreground Activity without allocating another native window. */
    public static void activate(android.app.Activity activity){WindowSession session=current;if(session!=null)session.attachActivity(activity);}
    /** Retire display callbacks after Activity/renderer teardown has completed. */
    public void stopCallbacks(Runnable ownerTasks){
        if(Thread.currentThread()!=owner)throw new IllegalStateException("Window session requires its owner thread");
        if(identity==0)return;
        if(!stopNative(identity,java.util.Objects.requireNonNull(ownerTasks)))
            throw new IllegalStateException("Window callbacks remain active during shutdown");
    }
    private static native long allocateNative();
    public static void hostRequestOrientation(android.app.Activity activity,boolean landscape){orientationNative(java.util.Objects.requireNonNull(activity),landscape);}
    private static native void orientationNative(android.app.Activity activity,boolean landscape);
    private static native void initializeNative(long identity,String title,int width,int height);
    private static native void attachActivityNative(long identity,android.app.Activity activity);
    private static native boolean closeNative(long identity);
    private static native boolean stopNative(long identity,Runnable ownerTasks);
}
