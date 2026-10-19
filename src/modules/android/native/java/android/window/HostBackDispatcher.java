package android.window;
public final class HostBackDispatcher implements OnBackInvokedDispatcher {
    private final java.util.LinkedHashMap<OnBackInvokedCallback,Integer> callbacks=new java.util.LinkedHashMap<>();
    private final Object lock=new Object();
    private final Thread owner=Thread.currentThread();
    private void check(){if(Thread.currentThread()!=owner)throw new IllegalStateException("Back dispatch requires the window owner thread");}
    // Like Android's WindowOnBackInvokedDispatcher, registration is protected
    // separately from dispatch. A renderer may register from its GL thread;
    // callbacks still execute on the thread that owns the window.
    public void registerOnBackInvokedCallback(int priority,OnBackInvokedCallback callback){
        java.util.Objects.requireNonNull(callback);
        if(priority<0)throw new IllegalArgumentException("Back priority must be nonnegative");
        synchronized(lock){
            if(!callbacks.containsKey(callback)&&callbacks.size()>=1024)throw new IllegalStateException("Too many Back callbacks");
            callbacks.remove(callback);callbacks.put(callback,priority);
        }
    }
    public void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback){
        java.util.Objects.requireNonNull(callback);
        synchronized(lock){callbacks.remove(callback);}
    }
    public boolean hostBack(){
        check();OnBackInvokedCallback selected=null;int priority=Integer.MIN_VALUE;
        synchronized(lock){
            for(var entry:callbacks.entrySet())if(entry.getValue()>=priority){selected=entry.getKey();priority=entry.getValue();}
        }
        // Selection reserves this invocation. Never hold the registry lock
        // while running guest code, including reentrant registration/removal.
        if(selected==null)return false;
        selected.onBackInvoked();return true;
    }
    public void hostClear(){check();synchronized(lock){callbacks.clear();}}
}
