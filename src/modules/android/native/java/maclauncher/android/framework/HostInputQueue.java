package maclauncher.android.framework;

import android.os.*;
import android.view.*;
import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.ArrayDeque;

/** NDK queue ingress; original InputQueue retains its active events and completion semantics. */
public final class HostInputQueue {
    private static final Method finish=HostUiAccess.method(InputQueue.class,"finishInputEvent",long.class,boolean.class);
    public static final class Owner {
        private final WeakReference<InputQueue> queue;
        private final Handler handler;
        private final ArrayDeque<Completion> completed=new ArrayDeque<>();
        private final Runnable dispatch=this::dispatch;
        private boolean queued,closed;
        private record Completion(long id,boolean handled){}
        Owner(WeakReference<InputQueue> queue,MessageQueue messages){this.queue=queue;Looper looper=HostLooper.forQueue(messages);if(looper==null)throw new IllegalStateException("Input queue has no owning Looper");
            // Native input acknowledgements normally arrive through the queue's
            // FD poller. Preserve that traversal-barrier-independent delivery.
            handler=Handler.createAsync(looper);
        }
        public synchronized void finish(long id,boolean handled){if(closed)return;completed.addLast(new Completion(id,handled));if(!queued){queued=handler.post(dispatch);if(!queued)completed.clear();}}
        private void dispatch(){
            for(;;){Completion completion;synchronized(this){completion=completed.pollFirst();if(completion==null||closed){queued=false;return;}}InputQueue owner=queue.get();if(owner!=null)HostUiAccess.call(finish,owner,completion.id,completion.handled);}
        }
        public synchronized void close(){closed=true;completed.clear();handler.removeCallbacks(dispatch);queued=false;}
    }
    public static synchronized long nativeInit(WeakReference<InputQueue> queue,MessageQueue messages){return create(new Owner(queue,messages));}
    public static long nativeSendKeyEvent(long handle,KeyEvent event,boolean preDispatch){return send(handle,new KeyEvent(event),1,preDispatch);}
    public static long nativeSendMotionEvent(long handle,MotionEvent event){MotionEvent copy=MotionEvent.obtain(event);try{return send(handle,copy,2,false);}catch(Throwable failure){copy.recycle();throw failure;}}
    public static void nativeDispose(long handle){dispose(handle);}
    private static native long create(Owner owner);
    private static native long send(long handle,InputEvent event,int type,boolean preDispatch);
    private static native void dispose(long handle);
}
