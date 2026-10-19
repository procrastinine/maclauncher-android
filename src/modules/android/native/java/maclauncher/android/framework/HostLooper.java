package maclauncher.android.framework;

import android.os.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
import java.util.*;

/** Window-loop adoption and retirement around the original Android message queue. */
public final class HostLooper {
    private HostLooper(){}
    private static final Field pointer=field("mPtr"),concurrent=field("mUseConcurrent"),quitting=field("mQuitting");
    private static final VarHandle quittingValue=quittingHandle();
    private static VarHandle quittingHandle(){try{return MethodHandles.privateLookupIn(MessageQueue.class,MethodHandles.lookup()).findVarHandle(MessageQueue.class,"mQuittingValue",boolean.class);}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    private static final MethodHandle once=handle(method(Looper.class,"loopOnce",Looper.class,long.class,int.class));
    private static final Method quit=method(MessageQueue.class,"quit",boolean.class),dispose=method(MessageQueue.class,"dispose");
    private static final ThreadLocal<Drain> draining=ThreadLocal.withInitial(Drain::new);
    private static final ThreadLocal<MessageQueue> closing=new ThreadLocal<>();
    private static final Map<MessageQueue,ArrayList<Runnable>> listeners=new WeakHashMap<>();
    private static final Map<MessageQueue,java.lang.ref.WeakReference<Looper>> owners=new WeakHashMap<>();
    private static volatile long mainDeadline=Long.MAX_VALUE;
    private static final class Drain {boolean active,boundaryReached;int polls;long deadline=Long.MAX_VALUE;}
    private static Field field(String name){try{Field result=MessageQueue.class.getDeclaredField(name);result.setAccessible(true);return result;}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    private static Method method(Class<?> owner,String name,Class<?>...types){try{Method result=owner.getDeclaredMethod(name,types);result.setAccessible(true);return result;}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    private static MethodHandle handle(Method method){try{return MethodHandles.lookup().unreflect(method);}catch(IllegalAccessException error){throw new ExceptionInInitializerError(error);}}
    private static RuntimeException failure(Throwable error){if(error instanceof RuntimeException value)return value;if(error instanceof Error value)throw value;return new IllegalStateException(error);}
    private static Object invoke(Method method,Object owner,Object...arguments){try{return method.invoke(owner,arguments);}catch(InvocationTargetException error){throw failure(error.getCause());}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}}
    private static long pointer(MessageQueue queue){try{return pointer.getLong(queue);}catch(IllegalAccessException error){throw new IllegalStateException(error);}}
    private static boolean quitting(MessageQueue queue){try{if(concurrent.getBoolean(queue))return (boolean)quittingValue.getVolatile(queue);synchronized(queue){return quitting.getBoolean(queue);}}catch(IllegalAccessException error){throw new IllegalStateException(error);}}

    public static Looper main(){
        synchronized(HostLooper.class){
            Looper value=Looper.getMainLooper();
            if(value==null){if(Looper.myLooper()!=null)throw new IllegalStateException("Window owner already has a non-main Looper");Looper.prepareMainLooper();value=Looper.getMainLooper();mainNative(pointer(value.getQueue()));}
            if(!value.isCurrentThread())throw new IllegalStateException("Main Looper belongs to another thread");return value;
        }
    }
    /** Called after the unchanged queue constructor has initialized its state. */
    public static void attachQueue(MessageQueue queue){attachNative(queue,pointer(queue));}
    /** Original Looper construction publishes the queue's actual dispatcher owner. */
    public static void attachLooper(Looper looper){synchronized(owners){owners.put(looper.getQueue(),new java.lang.ref.WeakReference<>(looper));}}
    public static Looper forQueue(MessageQueue queue){Objects.requireNonNull(queue);synchronized(owners){var owner=owners.get(queue);return owner==null?null:owner.get();}}
    /** A native poll in the SDL owner must yield when Android would block. */
    public static boolean poll(MessageQueue queue,long handle,int timeout){
        Drain current=draining.get();int result=pollNative(queue,handle,current.active?0:timeout);
        if(!current.active)return true;
        if(timeout>0){long now=SystemClock.uptimeMillis();current.deadline=Math.min(current.deadline,now+timeout);}
        if(++current.polls>=1024)return false;
        return timeout==0||result==-1||result==-2;
    }
    public static Message afterNext(MessageQueue queue,Message message){
        if(message==null&&quitting(queue)){invoke(dispose,queue);retire(queue);}return message;
    }
    /** Only explicit host teardown may stop Android's normally non-quittable main queue. */
    public static boolean quitAllowed(MessageQueue queue,boolean upstream){return upstream||closing.get()==queue;}
    /** Drain ready callbacks without waiting for future queue work. */
    public static void drain(Looper looper){
        drain(looper,true);
    }
    /** The physical JVM window pump also observes native poll deadlines. */
    public static void pump(Looper looper){
        drain(looper,false);
    }
    private static void drain(Looper looper,boolean callbacks){
        if(!looper.isCurrentThread())throw new IllegalStateException("Looper can only execute on its owner thread");
        Drain state=draining.get();if(state.active)return;state.active=true;state.boundaryReached=false;state.polls=0;state.deadline=Long.MAX_VALUE;
        // ART owns its original nativePollOnce. An asynchronous Handler marks
        // the ready-work boundary without a blocking poll or a sync-barrier wait.
        Handler boundary=callbacks?Handler.createAsync(looper):null;Runnable finish=callbacks?()->state.boundaryReached=true:null;
        // Match Looper.loop's two-step identity reset: its first clear may return
        // a foreign/uncleared token; only the second token describes this owner.
        try{if(boundary!=null&&!boundary.post(finish))return;Binder.clearCallingIdentity();long identity=Binder.clearCallingIdentity();for(int count=0;count<1024&&!state.boundaryReached;count++)if(!(boolean)once.invokeExact(looper,identity,-1))return;}
        catch(Throwable error){throw failure(error);}
        finally{if(boundary!=null)boundary.removeCallbacks(finish);state.active=false;if(!callbacks&&looper==Looper.getMainLooper())mainDeadline=state.polls>=1024?SystemClock.uptimeMillis():state.deadline;}
    }
    public static int waitMillis(int maximum){long remaining=mainDeadline-SystemClock.uptimeMillis();return remaining<=0?0:(int)Math.min(maximum,remaining);}
    public static boolean addShutdownListener(Looper looper,Runnable action){
        Objects.requireNonNull(action);MessageQueue queue=looper.getQueue();
        synchronized(listeners){if(quitting(queue)||pointer(queue)==0)return false;var entries=listeners.computeIfAbsent(queue,ignored->new ArrayList<>());for(Runnable old:entries)if(old==action)return true;entries.add(action);return true;}
    }
    public static void removeShutdownListener(Looper looper,Runnable action){synchronized(listeners){var entries=listeners.get(looper.getQueue());if(entries!=null)entries.removeIf(old->old==action);}}
    private static void retire(MessageQueue queue){
        synchronized(owners){owners.remove(queue);}
        List<Runnable> actions;synchronized(listeners){actions=listeners.remove(queue);}if(actions==null)return;
        Throwable first=null;for(Runnable action:actions)try{action.run();}catch(Throwable error){if(first==null)first=error;else if(first!=error)first.addSuppressed(error);}actions.clear();if(first!=null)throw failure(first);
    }
    /** Retire shared service listeners when an original VM owns queue disposal. */
    public static void retireCallbacks(Looper looper){
        if(looper==null)return;if(!looper.isCurrentThread())throw new IllegalStateException("Looper retirement requires its owner thread");retire(looper.getQueue());
    }
    public static void close(Looper looper){
        if(looper==null)return;if(!looper.isCurrentThread())throw new IllegalStateException("Looper retirement requires its owner thread");MessageQueue queue=looper.getQueue();Throwable first=null;MessageQueue prior=closing.get();closing.set(queue);
        try{invoke(quit,queue,false);}catch(Throwable error){first=error;}finally{if(prior==null)closing.remove();else closing.set(prior);}
        try{invoke(dispose,queue);}catch(Throwable error){if(first==null)first=error;else if(first!=error)first.addSuppressed(error);}
        try{retire(queue);}catch(Throwable error){if(first==null)first=error;else if(first!=error)first.addSuppressed(error);}
        if(looper==Looper.getMainLooper())mainDeadline=Long.MAX_VALUE;
        if(first!=null)throw failure(first);
    }
    public static void loopExit(){close(Looper.myLooper());}
    public static Throwable loopFailed(Throwable original){try{loopExit();}catch(Throwable cleanup){if(original!=cleanup)original.addSuppressed(cleanup);}return original;}
    private static native void attachNative(MessageQueue queue,long handle);
    private static native void mainNative(long handle);
    private static native int pollNative(MessageQueue queue,long handle,int timeout);
}
