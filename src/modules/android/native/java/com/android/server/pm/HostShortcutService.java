package com.android.server.pm;

import android.app.ActivityManager;
import android.app.IUidObserver;
import android.app.role.OnRoleHoldersChangedListener;
import android.content.Context;
import android.content.pm.ShortcutServiceInternal;
import android.os.*;
import com.android.server.LocalServices;
import com.android.server.SystemService;
import java.io.File;
import maclauncher.android.framework.*;

/** Physical storage and lifecycle bindings beneath the complete Android service. */
public final class HostShortcutService extends ShortcutService implements AutoCloseable {
    private final Context context;
    private final Looper worker;
    private final ShortcutServiceInternal local;
    private IUidObserver observer;
    private android.app.IActivityManager activityOwner;
    private OnRoleHoldersChangedListener roles;
    private boolean closed;
    public HostShortcutService(Context context,Looper worker){
        super(context,worker,false);this.context=context;this.worker=worker;
        local=LocalServices.getService(ShortcutServiceInternal.class);
    }
    public void start(){
        onBootPhase(SystemService.PHASE_LOCK_SETTINGS_READY);
        onBootPhase(SystemService.PHASE_BOOT_COMPLETED);
        handleUnlockUser(context.getUserId());
    }
    @Override File injectSystemDataPath(){return HostFilesystem.file(super.injectSystemDataPath());}
    @Override File injectUserDataPath(int userId){return HostFilesystem.file(super.injectUserDataPath(userId));}
    @Override void injectFinishWrite(ResilientAtomicFile file,java.io.FileOutputStream output)throws java.io.IOException{
        // Retain original atomic writes, fsync and reserve copy. APFS does not
        // implement Linux fs-verity; it cannot protect these files with that API.
        file.finishWrite(output,false);
    }
    @Override void injectRegisterUidObserver(IUidObserver value,int which){
        try{
            var shared=HostServices.platformService(Context.ACTIVITY_SERVICE);
            activityOwner=shared==null?ActivityManager.getService():android.app.IActivityManager.Stub.asInterface(shared);
            activityOwner.registerUidObserver(value,which,ActivityManager.PROCESS_STATE_UNKNOWN,null);observer=value;
        }catch(RemoteException failure){throw failure.rethrowFromSystemServer();}
    }
    @Override void injectRunOnNewThread(Runnable work){injectPostToHandler(work);}
    // No Android HOME-role surface is installed on the Mac desktop. Preserve the
    // original pin processor's absent-launcher result; no Mac shortcut is published.
    @Override void injectRegisterRoleHoldersListener(OnRoleHoldersChangedListener listener){
        if(ServiceManager.checkService(Context.ROLE_SERVICE)!=null){super.injectRegisterRoleHoldersListener(listener);roles=listener;}
    }
    @Override String injectGetHomeRoleHolderAsUser(int userId){return ServiceManager.checkService(Context.ROLE_SERVICE)==null?null:super.injectGetHomeRoleHolderAsUser(userId);}
    public void packageChanged(String action,String packageName,int uid){
        var intent=new android.content.Intent(action,android.net.Uri.fromParts("package",packageName,null))
            .putExtra(android.content.Intent.EXTRA_USER_HANDLE,UserHandle.getUserId(uid));
        injectPostToHandler(()->mPackageMonitor.onReceive(context,intent));
    }
    @Override public void close(){
        if(closed)return;closed=true;
        if(observer!=null)try{activityOwner.unregisterUidObserver(observer);}catch(RemoteException gone){}
        if(roles!=null)context.getSystemService(android.app.role.RoleManager.class).removeOnRoleHoldersChangedListenerAsUser(roles,UserHandle.ALL);
        HostApplication.releaseReceivers(context);
        synchronized(mServiceLock){
            var user=getShortcutsForTest().get(context.getUserId());
            handleStopUser(context.getUserId());
            // Original user serialization schedules package XML writes. Finish those
            // original writes before retiring the Handler and bitmap executors.
            if(user!=null)user.forAllPackageItems(item->{
                item.saveShortcutPackageItem();
                var executor=(java.util.concurrent.ExecutorService)field(item.mShortcutBitmapSaver,ShortcutBitmapSaver.class,"mExecutor");
                executor.shutdown();
            });
        }
        if(LocalServices.getService(ShortcutServiceInternal.class)==local)LocalServices.removeServiceForTest(ShortcutServiceInternal.class);
        ((Handler)field(this,ShortcutService.class,"mHandler")).removeCallbacksAndMessages(null);
    }
    private static Object field(Object owner,Class<?> type,String name){try{var value=type.getDeclaredField(name);value.setAccessible(true);return value.get(owner);}catch(ReflectiveOperationException failure){throw new IllegalStateException("Android shortcut lifetime field changed: "+name,failure);}}
}
