package maclauncher.android.framework;

import android.content.Context;
import android.content.pm.IShortcutService;
import android.os.*;
import android.os.Process;
import com.android.server.pm.HostShortcutService;
import com.android.server.usage.HostUsageEvents;

/** One profile-owned original shortcut service, shared by all application clients. */
public final class HostShortcuts implements AutoCloseable {
    private static final String DESCRIPTOR="android.content.pm.IShortcutService";
    private static HostShortcuts current;
    private final HandlerThread thread;
    private final HostShortcutService service;
    private final HostUsageEvents usage;
    private final android.content.pm.PackageManagerInternal.PackageListObserver packages=new android.content.pm.PackageManagerInternal.PackageListObserver(){
        public void onPackageAdded(String name,int uid){service.packageChanged(android.content.Intent.ACTION_PACKAGE_ADDED,name,uid);}
        public void onPackageRemoved(String name,int uid){service.packageChanged(android.content.Intent.ACTION_PACKAGE_REMOVED,name,uid);}
        public void onPackageChanged(String name,int uid){service.packageChanged(android.content.Intent.ACTION_PACKAGE_CHANGED,name,uid);}
    };
    private boolean closed;
    private HostShortcuts(Context context)throws RemoteException {
        thread=new HandlerThread("Android shortcuts",Process.THREAD_PRIORITY_BACKGROUND);thread.start();
        HostUsageEvents events=null;HostShortcutService created=null;
        try{
            events=HostUsageEvents.start(context,thread.getLooper());
            created=new HostShortcutService(context.createAttributionContext(context.getAttributionTag()),thread.getLooper());
            usage=events;service=created;
            service.start();
            ServiceManager.addService(Context.SHORTCUT_SERVICE,service);
            HostPackages.addPackageObserver(packages);
        }catch(Throwable failure){if(created!=null)created.close();if(events!=null)events.close();thread.quitSafely();throw failure;}
    }
    public static synchronized void start(Context context)throws RemoteException {
        if(current!=null)return;
        if(Process.myUid()!=Process.SYSTEM_UID)throw new SecurityException("Shortcuts require the Android system owner");
        HostShortcuts owner=new HostShortcuts(context);
        if(!HostLooper.addShutdownListener(context.getMainLooper(),owner::close)){owner.close();throw new IllegalStateException("Android profile service owner has stopped");}
        current=owner;
    }
    public static void remoteOwner(String packageName,String processName){
        var owner=new HostProcessServices.Key(packageName,processName);
        HostServices.registerLazy(Context.SHORTCUT_SERVICE,DESCRIPTOR,()->{
            try{IBinder binder=HostRemoteServices.systemService(owner,Context.SHORTCUT_SERVICE);if(binder==null)throw new DeadObjectException();return IShortcutService.Stub.asInterface(binder);}
            catch(RemoteException failure){throw failure.rethrowFromSystemServer();}
        });
    }
    @Override public synchronized void close(){
        if(closed)return;closed=true;
        HostServices.unregister(Context.SHORTCUT_SERVICE,service,DESCRIPTOR);
        HostPackages.removePackageObserver(packages);
        try{service.close();}finally{try{usage.close();}finally{thread.quitSafely();}}
        if(thread!=Thread.currentThread())try{thread.join();}catch(InterruptedException interrupted){Thread.currentThread().interrupt();}
        synchronized(HostShortcuts.class){if(current==this)current=null;}
    }
}
