package com.android.server.locales;

import android.app.ILocaleManager;
import android.content.Context;
import android.os.*;
import com.android.server.LocalServices;
import com.android.server.wm.HostPackageConfiguration;
import maclauncher.android.framework.*;

/** Owns the unchanged LocaleManager service and its original helper lifetimes. */
public final class HostLocaleService implements AutoCloseable {
    private final Context context;
    private HandlerThread thread;
    private final HostPackageConfiguration configurations;
    private LocaleManagerService service;
    private LocaleManagerBackupHelper backup;
    private LocaleManagerServicePackageMonitor monitor;
    private ILocaleManager endpoint;
    private AutoCloseable packages;
    private volatile boolean closed;
    private boolean monitorRegistered,started,threadStarted;
    public HostLocaleService(Context context){
        if(android.os.Process.myUid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("Locale service requires the Android system owner");
        this.context=context;
        // Android init/system-server normally establish these real system stores.
        // Both hosted VMs retain their original Environment namespace and XML owners.
        try{for(var directory:new java.io.File[]{Environment.getDataSystemDirectory(),Environment.getDataSystemCeDirectory(context.getUserId()),Environment.getDataSystemDeDirectory(context.getUserId())})java.nio.file.Files.createDirectories(directory.toPath());}
        catch(java.io.IOException failure){throw new IllegalStateException("Android locale storage could not be opened",failure);}
        configurations=new HostPackageConfiguration(context.getUserId(),HostLocaleConfiguration::changed);
        try{
        thread=new HandlerThread("Android locales",android.os.Process.THREAD_PRIORITY_BACKGROUND);thread.start();threadStarted=true;
        // The original injectable constructor preserves all service methods; only
        // system-server internal owners are supplied by the hosting process.
        service=new LocaleManagerService(context,null,null,context.getPackageManager(),null,null);
        SystemAppUpdateTracker tracker=new SystemAppUpdateTracker(service);
        backup=new LocaleManagerBackupHelper(service,context.getPackageManager(),thread);
        monitor=new LocaleManagerServicePackageMonitor(backup,tracker,service);
        set(service,"mBackupHelper",backup);
        set(service,"mPackageMonitor",monitor);
        endpoint=(ILocaleManager)get(service,"mBinderService");
            monitor.register(context,thread.getLooper(),UserHandle.ALL,true);monitorRegistered=true;
            thread.getThreadHandler().postAtFrontOfQueue(tracker::init);
            service.onStart();started=true;packages=HostLocaleConfiguration.packages(this);
        }catch(RuntimeException|Error failure){try{close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }
    private static Object get(Object owner,String name){try{var field=LocaleManagerService.class.getDeclaredField(name);field.setAccessible(true);return field.get(owner);}catch(ReflectiveOperationException failure){throw new IllegalStateException("Original locale service field changed",failure);}}
    private static void set(Object owner,String name,Object value){try{var field=LocaleManagerService.class.getDeclaredField(name);field.setAccessible(true);field.set(owner,value);}catch(ReflectiveOperationException failure){throw new IllegalStateException("Original locale service field changed",failure);}}
    private void dispatch(Runnable operation){
        if(closed)return;
        if(!thread.getThreadHandler().postAtTime(()->{if(!closed)operation.run();},this,SystemClock.uptimeMillis())&&!closed)throw new IllegalStateException("Android locale handler has stopped");
    }
    public void packageAdded(String name,int uid){dispatch(()->monitor.onPackageAddedWithExtras(name,uid,new Bundle()));}
    public void packageRemoved(String name,int uid){dispatch(()->{monitor.onPackageRemoved(name,uid);configurations.packageRemoved(name,UserHandle.getUserId(uid));});}
    public void packageChanged(String name,int uid){dispatch(()->monitor.onPackageUpdateFinished(name,uid));}
    public ILocaleManager endpoint(){if(closed)throw new IllegalStateException("Android locale service is closed");return endpoint;}
    public void packageDataCleared(String name,int uid){dispatch(()->{monitor.onPackageDataCleared(name,uid);configurations.packageDataCleared(name,UserHandle.getUserId(uid));});}
    private static Throwable retire(Throwable first,AutoCloseable operation){try{operation.close();}catch(Throwable failure){if(first==null)return failure;if(first!=failure)first.addSuppressed(failure);}return first;}
    @Override public void close(){
        if(closed)return;closed=true;Throwable failure=null;
        if(endpoint!=null)failure=retire(failure,()->HostServices.unregister(Context.LOCALE_SERVICE,endpoint,ILocaleManager.DESCRIPTOR));
        if(packages!=null)failure=retire(failure,packages);
        if(started)failure=retire(failure,()->LocalServices.removeServiceForTest(LocaleManagerInternal.class));
        if(monitorRegistered)failure=retire(failure,monitor::unregister);
        if(backup!=null)failure=retire(failure,()->context.unregisterReceiver(backup.getUserMonitor()));
        if(threadStarted){thread.getThreadHandler().removeCallbacksAndMessages(this);thread.quitSafely();if(thread!=Thread.currentThread())failure=retire(failure,()->{try{thread.join();}catch(InterruptedException interrupted){Thread.currentThread().interrupt();throw interrupted;}});}
        failure=retire(failure,configurations);
        if(failure!=null)throw new IllegalStateException("Android locale service could not retire",failure);
    }
}
