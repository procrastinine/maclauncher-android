package maclauncher.android.framework;

import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.PackageManagerInternal;
import android.media.IAudioPolicyService;
import android.os.*;
import android.permission.IOnPermissionsChangeListener;
import android.permission.IPermissionManager;
import com.android.server.audio.AudioServerPermissionProvider;
import com.android.media.permission.INativePermissionController;
import com.android.media.permission.UidPackageState;
import java.util.*;
import com.android.server.pm.pkg.PackageState;
import java.util.concurrent.*;

/** Connects the original native audio permission provider to the profile's existing owners. */
final class HostAudioPermissions implements AutoCloseable {
    private static final String SERVICE="media.audio_policy";
    private volatile Thread ownerThread;
    private volatile boolean closed;
    private final ExecutorService updates=Executors.newSingleThreadExecutor(task->{
        Thread thread=new Thread(task,"Android audio permissions");thread.setDaemon(true);ownerThread=thread;return thread;
    });
    private final AudioServerPermissionProvider provider;
    private record Destination(IBinder policy,INativePermissionController controller,IBinder.DeathRecipient death){}
    // These are transport leases only. All package/grant state and replay stay
    // in the one original AudioServerPermissionProvider below.
    private final Map<HostBinderRpc.Peer,Destination> destinations=new HashMap<>();
    private final INativePermissionController fanout=new INativePermissionController.Stub(){
        public void populatePackagesForUids(List<UidPackageState> values){broadcast(target->target.populatePackagesForUids(values));}
        public void updatePackagesForUid(UidPackageState value){broadcast(target->target.updatePackagesForUid(value));}
        public void populatePermissionState(byte permission,int[] uids){broadcast(target->target.populatePermissionState(permission,uids));}
    };
    @FunctionalInterface private interface Update{void send(INativePermissionController target)throws RemoteException;}
    private void broadcast(Update update){
        for(var entry:List.copyOf(destinations.entrySet()))try{update.send(entry.getValue().controller());}
        catch(RemoteException gone){retire(entry.getKey(),entry.getValue().policy());}
    }
    private void retire(HostBinderRpc.Peer peer,IBinder expected){
        var value=destinations.get(peer);if(value==null||value.policy()!=expected)return;
        destinations.remove(peer);try{value.policy().unlinkToDeath(value.death(),0);}catch(NoSuchElementException alreadyDead){}
    }
    private void changed(HostServices.ScopedService entry){
        if(closed)return;var previous=destinations.get(entry.peer());
        if(previous!=null&&previous.policy()==entry.binder())return;
        if(previous!=null)retire(entry.peer(),previous.policy());
        if(entry.binder()==null)return;
        try{
            var service=IAudioPolicyService.Stub.asInterface(entry.binder());
            var target=service.getPermissionController();if(target==null)throw new IllegalStateException("Native audio permission controller is unavailable");
            IBinder.DeathRecipient death=()->enqueue(()->retire(entry.peer(),entry.binder()));
            entry.binder().linkToDeath(death,0);
            destinations.put(entry.peer(),new Destination(entry.binder(),target,death));
            provider.onServiceStart(fanout);
        }catch(RemoteException gone){retire(entry.peer(),entry.binder());}
    }
    private final IPermissionManager permissions;
    private AutoCloseable registrations;
    private AutoCloseable published;
    private final PackageManagerInternal.PackageListObserver packages=new PackageManagerInternal.PackageListObserver(){
        @Override public void onPackageAdded(String name,int uid){changed(name,uid,false);}
        @Override public void onPackageRemoved(String name,int uid){changed(name,uid,true);}
        @Override public void onPackageChanged(String name,int uid){changed(name,uid,false);}
        private void changed(String name,int uid,boolean removed){enqueue(()->{provider.onModifyPackageState(uid,name,removed);provider.onPermissionStateChanged();});}
    };
    private final IOnPermissionsChangeListener grants=new IOnPermissionsChangeListener.Stub(){
        @Override public void onPermissionsChanged(int uid,String device){enqueue(provider::onPermissionStateChanged);}
    };
    HostAudioPermissions(Context context)throws Exception {
        synchronized(HostPackages.class){
            provider=new AudioServerPermissionProvider(new ArrayList<PackageState>(HostPackages.hostGetPackageStates(null).values()),
                (uid,permission)->HostPermissions.check(permission,uid,Context.DEVICE_ID_DEFAULT)==PackageManager.PERMISSION_GRANTED,
                ()->HostUserService.hostGetUserIds(null));
            HostPackages.addPackageObserver(packages);
        }
        permissions=IPermissionManager.Stub.asInterface(HostServices.manager().checkService("permissionmgr"));
        try{
            permissions.addOnPermissionsChangeListener(grants);
            registrations=HostServices.observeScopes(SERVICE,entry->enqueue(()->changed(entry)));
            published=maclauncher.android.media.HostAudioService.publishNative(this::barrier);
        }catch(Throwable failure){try{close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }
    private void enqueue(Runnable operation){
        if(closed)return;
        try{updates.execute(()->{if(!closed)operation.run();});}
        catch(RejectedExecutionException failure){if(!closed)throw failure;}
    }
    private void barrier(){
        if(closed)throw new IllegalStateException("Android audio permission owner has stopped");
        if(Thread.currentThread()==ownerThread){provider.onPermissionStateChanged();return;}
        try{updates.submit(provider::onPermissionStateChanged).get();}
        catch(InterruptedException failure){Thread.currentThread().interrupt();throw new IllegalStateException("Audio permission synchronization interrupted",failure);}
        catch(ExecutionException failure){throw new IllegalStateException("Audio permission synchronization failed",failure.getCause());}
    }
    @Override public void close() {
        if(closed)return;closed=true;
        HostPackages.removePackageObserver(packages);
        try{
            permissions.removeOnPermissionsChangeListener(grants);
            if(registrations!=null)registrations.close();
        }catch(Exception failure){throw new IllegalStateException("Audio permission retirement failed",failure);}
        finally{
            // Retire after the original provider's last serialized update.
            Runnable retireAll=()->{for(var entry:List.copyOf(destinations.entrySet()))retire(entry.getKey(),entry.getValue().policy());};
            if(Thread.currentThread()==ownerThread)retireAll.run();else updates.execute(retireAll);
            updates.shutdown();
            try{if(Thread.currentThread()!=ownerThread)updates.awaitTermination(2,TimeUnit.SECONDS);}
            catch(InterruptedException interrupted){Thread.currentThread().interrupt();}
            finally{
                try{if(published!=null)published.close();}
                catch(Exception failure){throw new IllegalStateException("Audio service retirement failed",failure);}
            }
        }
    }
}
