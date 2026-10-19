package com.android.server.usage;

import android.app.usage.UsageEvents;
import android.app.usage.UsageStatsManagerInternal;
import android.content.Context;
import android.os.*;
import java.io.File;
import java.util.HashMap;
import maclauncher.android.framework.HostFilesystem;

/** Process lifetime and private storage for the original usage-event database. */
public final class HostUsageEvents implements AutoCloseable {
    private static HostUsageEvents current;
    private final UserUsageStatsService original;
    private final int user;
    private final Handler handler;
    private final Runnable save=this::flush;
    private boolean closed;
    private HostUsageEvents(Context context,Looper looper)throws android.os.RemoteException {
        user=context.getUserId();handler=new Handler(looper);
        File directory=HostFilesystem.file(new File(Environment.getDataSystemCeDirectory(user),"usagestats"));
        original=new UserUsageStatsService(context,user,directory,new UserUsageStatsService.StatsUpdatedListener(){
            public void onStatsUpdated(){handler.removeCallbacks(save);handler.postDelayed(save,60_000);}
            public void onStatsReloaded(){}
            public void onNewUpdate(int userId){}
        });
        var installed=new HashMap<String,Long>();
        for(Object value:android.app.AppGlobals.getPackageManager().getInstalledPackages(0,user).getList()){
            var info=(android.content.pm.PackageInfo)value;installed.put(info.packageName,info.firstInstallTime);
        }
        original.init(System.currentTimeMillis(),installed,false);
    }
    public static synchronized HostUsageEvents start(Context context,Looper looper)throws android.os.RemoteException {
        if(current!=null)throw new IllegalStateException("Android usage-event owner is already running");
        return current=new HostUsageEvents(context,looper);
    }
    public static void reportShortcutUsage(UsageStatsManagerInternal service,String packageName,String shortcutId,int userId){
        if(service!=null){service.reportShortcutUsage(packageName,shortcutId,userId);return;}
        synchronized(HostUsageEvents.class){
            var owner=current;if(owner==null||owner.closed)throw new IllegalStateException("Android usage-event owner has stopped");
            if(owner.user!=userId)throw new SecurityException("Other Android users are unavailable");
            var event=new UsageEvents.Event();event.mEventType=UsageEvents.Event.SHORTCUT_INVOCATION;
            event.mPackage=packageName;event.mShortcutId=shortcutId;event.mTimeStamp=SystemClock.elapsedRealtime();
            owner.original.reportEvent(event);
        }
    }
    public void flush(){synchronized(HostUsageEvents.class){if(!closed)original.persistActiveStats();}}
    @Override public void close(){synchronized(HostUsageEvents.class){if(closed)return;handler.removeCallbacks(save);original.persistActiveStats();closed=true;if(current==this)current=null;}}
}
