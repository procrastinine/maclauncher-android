package maclauncher.android.framework;

import android.graphics.BLASTBufferQueue;
import android.os.IBinder;
import android.view.Surface;
import android.view.SurfaceControl;
import java.util.*;
import java.util.function.*;

/** Local buffer-acquisition boundary below the original AOSP BLAST client. */
public final class HostBufferQueue {
    private HostBufferQueue(){}
    private static final class Queue implements AutoCloseable {
        final String name;
        final NavigableMap<Long,SurfaceControl.Transaction> pending=new TreeMap<>();
        final LongConsumer acquired=this::acquired;
        long control,buffer,lastFrame;
        Consumer<SurfaceControl.Transaction> sync;
        boolean continuous,closed;
        BLASTBufferQueue.TransactionHangCallback hang;
        BLASTBufferQueue.WaitForBufferReleaseCallback release;
        Queue(String name){this.name=name;}
        void acquired(long frame){
            lastFrame=frame;var transactions=gather(frame);
            if(sync!=null){var callback=sync;if(!continuous)sync=null;callback.accept(transactions);}
            else try(transactions){transactions.apply();}
        }
        SurfaceControl.Transaction gather(long frame){
            var result=new SurfaceControl.Transaction();
            for(var iterator=pending.entrySet().iterator();iterator.hasNext();){var entry=iterator.next();if(entry.getKey()>frame)break;try(var transaction=entry.getValue()){result.merge(transaction);}iterator.remove();}
            return result;
        }
        void detach(){if(buffer!=0){long previous=buffer;buffer=0;HostSurface.buffer(previous).frames.remove(acquired);HostNativeHandle.release(previous);}if(control!=0){long previous=control;control=0;HostNativeHandle.release(previous);}}
        @Override public void close(){synchronized(HostSurfaceControl.scene){if(closed)return;closed=true;sync=null;hang=null;release=null;try{for(var value:pending.values())value.close();}finally{pending.clear();detach();}}}
    }
    private static Queue queue(long handle){return (Queue)HostNativeHandle.state(handle);}
    public static long nativeCreate(String name,boolean updateDestinationFrame){return HostNativeHandle.create(new Queue(name));}
    public static void nativeDestroy(long handle){HostNativeHandle.release(handle);}
    static long bufferHandle(long handle){synchronized(HostSurfaceControl.scene){var owner=queue(handle);if(owner.buffer==0)throw new IllegalStateException("Buffer queue has no surface");return owner.buffer;}}
    public static Surface nativeGetSurface(long handle,boolean withHandle){synchronized(HostSurfaceControl.scene){var surface=new Surface();long buffer=HostNativeHandle.retain(bufferHandle(handle));try{HostSurface.assign(surface,buffer);return surface;}catch(Throwable failure){HostNativeHandle.release(buffer);throw failure;}}}
    public static boolean nativeSyncNextTransaction(long handle,Consumer<SurfaceControl.Transaction> callback,boolean acquireSingleBuffer){synchronized(HostSurfaceControl.scene){var owner=queue(handle);if(owner.sync!=null)return false;owner.sync=Objects.requireNonNull(callback);owner.continuous=!acquireSingleBuffer;return true;}}
    public static void nativeStopContinuousSyncTransaction(long handle){synchronized(HostSurfaceControl.scene){var owner=queue(handle);owner.continuous=false;owner.sync=null;}}
    public static void nativeClearSyncTransaction(long handle){nativeStopContinuousSyncTransaction(handle);}
    public static void nativeUpdate(long handle,long control,long width,long height,int format){synchronized(HostSurfaceControl.scene){
        var owner=queue(handle);if(width<1||height<1||width>8192||height>8192)throw new IllegalArgumentException("Surface buffer dimensions are unsupported");
        HostSurfaceControl.nativeUpdateDefaultBufferSize(control,(int)width,(int)height);
        if(owner.control==control)return;
        long next=HostSurfaceControl.acquireBuffer(control);HostNativeHandle.retain(control);
        owner.detach();owner.control=control;owner.buffer=next;HostSurface.buffer(next).frames.add(owner.acquired);
    }}
    public static void nativeMergeWithNextTransaction(long handle,long transaction,long frame){synchronized(HostSurfaceControl.scene){
        var owner=queue(handle);var target=owner.pending.computeIfAbsent(frame,unused->new SurfaceControl.Transaction());
        HostSurfaceControl.nativeMergeTransaction(target.mNativeObject,transaction);
        if(frame<=owner.lastFrame)nativeApplyPendingTransactions(handle,owner.lastFrame);
    }}
    public static long nativeGetLastAcquiredFrameNum(long handle){synchronized(HostSurfaceControl.scene){return queue(handle).lastFrame;}}
    public static void nativeApplyPendingTransactions(long handle,long frame){synchronized(HostSurfaceControl.scene){try(var transaction=queue(handle).gather(frame)){transaction.apply();}}}
    public static boolean nativeIsSameSurfaceControl(long handle,long control){synchronized(HostSurfaceControl.scene){return queue(handle).control==control;}}
    public static SurfaceControl.Transaction nativeGatherPendingTransactions(long handle,long frame){synchronized(HostSurfaceControl.scene){return queue(handle).gather(frame);}}
    public static void nativeSetTransactionHangCallback(long handle,BLASTBufferQueue.TransactionHangCallback callback){synchronized(HostSurfaceControl.scene){queue(handle).hang=callback;}}
    public static void nativeSetApplyToken(long handle,IBinder token){queue(handle);/* Local transactions have one in-process owner and no Binder apply queue. */}
    public static void nativeSetWaitForBufferReleaseCallback(long handle,BLASTBufferQueue.WaitForBufferReleaseCallback callback){synchronized(HostSurfaceControl.scene){queue(handle).release=callback;}}
}
