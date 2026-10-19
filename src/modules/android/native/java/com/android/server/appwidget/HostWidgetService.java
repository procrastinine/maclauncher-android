package com.android.server.appwidget;

import android.appwidget.AppWidgetManagerInternal;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import com.android.server.LocalServices;
import maclauncher.android.framework.HostApplication;

/** Process lifetime binding for the complete supplied Android widget service. */
public final class HostWidgetService extends AppWidgetServiceImpl implements AutoCloseable {
    private final Context context;
    private AppWidgetManagerInternal local;
    private boolean closed;
    public HostWidgetService(Context context){
        super(context);this.context=context;
        try{onStart();local=LocalServices.getService(AppWidgetManagerInternal.class);}
        catch(RuntimeException|Error failure){try{close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }
    @Override public synchronized void close(){
        if(closed)return;closed=true;
        HostApplication.releaseReceivers(context);
        if(local!=null&&LocalServices.getService(AppWidgetManagerInternal.class)==local)
            LocalServices.removeServiceForTest(AppWidgetManagerInternal.class);
        Handler callback=(Handler)field("mCallbackHandler");
        if(callback!=null){
            callback.removeCallbacksAndMessages(null);
            Looper looper=callback.getLooper();looper.quitSafely();
            if(looper.getThread()!=Thread.currentThread())try{looper.getThread().join(2000);}
            catch(InterruptedException interrupted){Thread.currentThread().interrupt();}
        }
    }
    private Object field(String name){
        try{var field=AppWidgetServiceImpl.class.getDeclaredField(name);field.setAccessible(true);return field.get(this);}
        catch(ReflectiveOperationException failure){throw new IllegalStateException("Android widget lifetime field changed: "+name,failure);}
    }
}
