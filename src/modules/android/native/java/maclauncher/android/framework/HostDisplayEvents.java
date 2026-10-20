package maclauncher.android.framework;

import android.os.Handler;
import android.os.Looper;
import android.os.MessageQueue;
import android.view.DisplayEventReceiver;
import android.view.DisplayEventReceiver.VsyncEventData;
import java.lang.invoke.*;
import java.lang.ref.WeakReference;
import java.util.*;

/** CVDisplayLink transport below AOSP DisplayEventReceiver and Choreographer.
 * Android owns callback queues, animation phases, delays and frame scheduling. */
public final class HostDisplayEvents {
    private static final Object lock=new Object();
    private static final Set<Receiver> receivers=Collections.newSetFromMap(new IdentityHashMap<>());
    private static final Set<Runnable> presentations=Collections.newSetFromMap(new IdentityHashMap<>());
    private static final MethodHandle newData;
    private static Runnable wakeup;
    private static Thread hostOwner;
    private static boolean stopped;
    private static int callbacks;
    private static Tick latest;
    private record Tick(long sequence,long callback,long output,long interval,long display) {}
    static {
        try{newData=MethodHandles.privateLookupIn(VsyncEventData.class,MethodHandles.lookup()).findConstructor(VsyncEventData.class,MethodType.methodType(void.class));}
        catch(ReflectiveOperationException failure){throw new ExceptionInInitializerError(failure);}
    }
    private HostDisplayEvents(){}
    private static VsyncEventData data(){
        try{return (VsyncEventData)newData.invokeExact();}
        catch(RuntimeException|Error failure){throw failure;}catch(Throwable failure){throw new IllegalStateException(failure);}
    }
    private static void update(VsyncEventData data,Tick tick){
        if(tick==null)return;
        data.frameInterval=tick.interval;data.preferredFrameTimelineIndex=0;data.frameTimelinesLength=1;data.numberQueuedBuffers=0;
        var timeline=data.frameTimelines[0];timeline.vsyncId=tick.sequence;timeline.expectedPresentationTime=tick.output;
        timeline.deadline=tick.output;
    }
    private static final class Receiver implements AutoCloseable {
        final WeakReference<DisplayEventReceiver> receiver;
        final WeakReference<VsyncEventData> eventData;
        final Looper looper;
        final Handler handler;
        final Runnable dispatch=this::dispatch,retire=this::close;
        boolean requested,queued,closed;
        Tick tick;
        Receiver(WeakReference<DisplayEventReceiver> receiver,WeakReference<VsyncEventData> data,Looper looper){
            this.receiver=receiver;eventData=data;this.looper=looper;handler=Handler.createAsync(looper);
        }
        void accept(Tick next){synchronized(lock){
            if(closed||!requested)return;tick=next;requested=false;
            if(!queued){queued=true;if(!handler.post(dispatch)){queued=false;close();}}
        }}
        void dispatch(){
            DisplayEventReceiver target;VsyncEventData data;Tick frame;
            synchronized(lock){queued=false;if(closed||stopped)return;if(wakeup==null){requested=true;tick=null;return;}target=receiver.get();data=eventData.get();frame=tick;
                if(target==null||data==null){close();return;}callbacks++;update(data,frame);}
            try{target.onVsync(frame.callback,frame.display,(int)(frame.sequence&0x7fffffff),data);}
            finally{leaveCallback();}
        }
        @Override public void close(){synchronized(lock){
            if(closed)return;closed=true;requested=queued=false;tick=null;handler.removeCallbacks(dispatch);receivers.remove(this);
            HostLooper.removeShutdownListener(looper,retire);signal();
        }}
    }
    public static long nativeInit(WeakReference<DisplayEventReceiver> receiver,WeakReference<VsyncEventData> data,MessageQueue queue,int source,int flags,long layer){
        Looper looper=HostLooper.forQueue(queue);if(looper==null)throw new IllegalStateException("Display event queue has no active Looper");
        var owner=new Receiver(receiver,data,looper);
        synchronized(lock){
            if(stopped)throw new IllegalStateException("Display source has stopped");
            if(!HostLooper.addShutdownListener(looper,owner.retire))throw new IllegalStateException("Display event queue has stopped");
            receivers.add(owner);
            try{return HostNativeHandle.create(owner);}catch(Throwable failure){owner.close();throw failure;}
        }
    }
    public static long nativeGetDisplayEventReceiverFinalizer(){return HostNativeHandle.finalizer();}
    public static void nativeScheduleVsync(long handle){synchronized(lock){var owner=(Receiver)HostNativeHandle.state(handle);if(!owner.closed&&!stopped){owner.requested=true;signal();}}}
    public static VsyncEventData nativeGetLatestVsyncEventData(long handle){HostNativeHandle.state(handle);var result=data();synchronized(lock){update(result,latest);}return result;}
    /** Retry a busy presenter on the next actual display tick, with one entry per output. */
    static void requestPresentation(Runnable work){synchronized(lock){if(!stopped){presentations.add(work);signal();}}}
    static void cancelPresentation(Runnable work){synchronized(lock){presentations.remove(work);}}
    /** Demand is read by the display dispatcher even during a blocking Activity callback. */
    public static boolean hostNeedsFrames(){synchronized(lock){if(stopped)return false;if(!presentations.isEmpty())return true;for(var owner:receivers)if(owner.requested)return true;return false;}}
    public static void hostSetWakeup(Runnable signal){synchronized(lock){checkHost();if(stopped||wakeup!=null)throw new IllegalStateException("Display wake source is already configured or stopped");wakeup=Objects.requireNonNull(signal);signal();}}
    private static void signal(){if(wakeup!=null&&!stopped)wakeup.run();}
    public static int hostWaitMillis(int maximum){synchronized(lock){checkHost();}return HostLooper.waitMillis(maximum);}
    /** Publish from the display dispatcher; each original receiver keeps its own Looper. */
    public static void hostDispatchFrame(long sequence,long callback,long output,long interval,long display){
        Receiver[] targets;Tick tick=new Tick(sequence,callback,output,interval,display);
        synchronized(lock){
            if(stopped||wakeup==null)return;
            if(callback<=0||callback>System.nanoTime()||interval<=0)throw new IllegalArgumentException("Invalid native display timestamp");
            if(latest!=null&&callback<=latest.callback)return;latest=tick;HostLooper.hostFrameInterval(interval);targets=receivers.toArray(Receiver[]::new);
        }
        for(var target:targets)target.accept(tick);
    }
    /** Retry presentation only on the native UI owner, independently of receiver delivery. */
    public static void hostPresentFrame(){
        Runnable[] outputs;synchronized(lock){checkHost();if(stopped)return;outputs=presentations.toArray(Runnable[]::new);presentations.clear();}
        for(var work:outputs)work.run();
    }
    /** Protect native teardown while an original AOSP doFrame is executing. */
    public static boolean enterCallback(){synchronized(lock){if(stopped)return false;callbacks++;return true;}}
    public static void leaveCallback(){synchronized(lock){callbacks--;}}
    public static boolean hostHasInFlightCallbacks(){synchronized(lock){if(callbacks!=0)return true;for(var owner:receivers)if(owner.queued)return true;return false;}}
    /** Retire a native output without destroying process-owned AOSP receivers. */
    public static void hostDetachSource(){synchronized(lock){
        checkHost();wakeup=null;latest=null;presentations.clear();
        for(var receiver:receivers){if(receiver.queued){receiver.handler.removeCallbacks(receiver.dispatch);receiver.requested=true;receiver.queued=false;}receiver.tick=null;}
    }}
    public static void hostShutdown(){Receiver[] targets;synchronized(lock){checkHost();stopped=true;wakeup=null;presentations.clear();targets=receivers.toArray(Receiver[]::new);}for(var target:targets)target.close();}
    private static void checkHost(){if(hostOwner==null)hostOwner=Thread.currentThread();else if(hostOwner!=Thread.currentThread())throw new IllegalStateException("Display source belongs to another host thread");}
}
