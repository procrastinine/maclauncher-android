package maclauncher.android.framework;

import android.os.AsyncTask;
import android.os.Looper;
import java.lang.reflect.Field;
import java.util.*;
import java.util.concurrent.*;

/** Application teardown for the original AsyncTask executors and task state. */
public final class HostAsyncTasks {
    private static final Map<AsyncTask<?,?,?>,Boolean> tasks=new WeakHashMap<>();
    private static boolean attached,closed;
    private HostAsyncTasks(){}
    public static void attachExecutors(){synchronized(tasks){
        if(closed)throw new RejectedExecutionException("Application task owner has closed");
        if(!attached){if(!HostLooper.addShutdownListener(Looper.getMainLooper(),HostAsyncTasks::close))throw new RejectedExecutionException("Application queue has closed");attached=true;}
    }}
    public static void attach(AsyncTask<?,?,?> task){synchronized(tasks){
        attachExecutors();
        tasks.put(task,Boolean.TRUE);
    }}
    public static void finished(AsyncTask<?,?,?> task){synchronized(tasks){tasks.remove(task);}}
    public static void shutdown(){close();}
    private static Object field(Class<?> type,Object owner,String name){try{Field field=type.getDeclaredField(name);field.setAccessible(true);return field.get(owner);}catch(ReflectiveOperationException failure){throw new IllegalStateException("Android executor ownership is unavailable",failure);}}
    private static void close(){
        List<AsyncTask<?,?,?>> owned;synchronized(tasks){if(closed)return;closed=true;owned=List.copyOf(tasks.keySet());tasks.clear();}
        // Clear Android's actual serial queue under its own monitor. The active
        // wrapper retains Android's scheduleNext and cancellation algorithms.
        Executor serial=AsyncTask.SERIAL_EXECUTOR;
        synchronized(serial){((Deque<?>)field(serial.getClass(),serial,"mTasks")).clear();}
        for(var task:owned)task.cancel(true);
        Set<ExecutorService> executors=Collections.newSetFromMap(new IdentityHashMap<>());
        if(AsyncTask.THREAD_POOL_EXECUTOR instanceof ExecutorService pool)executors.add(pool);
        Object backup=field(AsyncTask.class,null,"sBackupExecutor");if(backup instanceof ExecutorService pool)executors.add(pool);
        for(var executor:executors)for(var queued:executor.shutdownNow())if(queued instanceof Future<?> future)future.cancel(false);
    }
}
