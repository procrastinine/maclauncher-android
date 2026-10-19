package maclauncher.android.framework;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.DeadObjectException;
import android.app.IActivityManager;
import android.app.IUriGrantsManager;
import com.android.server.LocalServices;
import com.android.server.SystemService;
import com.android.server.uri.GrantUri;
import com.android.server.uri.UriGrantsManagerInternal;
import com.android.server.uri.UriGrantsManagerService;

/** Process lifetime of Android's original URI-grant service. The system account
 * owner retains the only grant tree, persistence file, owner tokens and pruning
 * algorithms. Host calls below delegate to that original Internal contract. */
public final class HostUriGrants {
    private static UriGrantsManagerService service;
    private static volatile UriGrantsManagerInternal local;
    private static volatile HostProcessServices.Key remote;
    private HostUriGrants(){}
    /** Reuse the existing authenticated system-process connection. Clients keep
     * their local ActivityManager; only URI policy delegates to its sole owner. */
    public static void remoteOwner(String packageName,String processName){
        remote=new HostProcessServices.Key(packageName,processName);
        HostServices.registerLazy(Context.URI_GRANTS_SERVICE,IUriGrantsManager.DESCRIPTOR,()->{
            try{IBinder binder=HostRemoteServices.systemService(remote,Context.URI_GRANTS_SERVICE);
                if(binder==null)throw new DeadObjectException();return IUriGrantsManager.Stub.asInterface(binder);
            }catch(RemoteException failure){throw failure.rethrowFromSystemServer();}
        });
    }
    private static IActivityManager remote()throws RemoteException{
        var peer=remote;if(peer==null)return null;
        IBinder binder=HostRemoteServices.systemService(peer,Context.ACTIVITY_SERVICE);
        if(binder==null)throw new DeadObjectException();return IActivityManager.Stub.asInterface(binder);
    }
    private static int caller(){
        int uid=Binder.getCallingUid();
        if(android.os.Process.isIsolated(uid))throw new SecurityException("Isolated processes cannot manage URI permissions");
        var peer=HostBinderRpc.callingPeer();
        if(peer!=null&&(peer.uid()!=uid||peer.pid()!=Binder.getCallingPid()))throw new SecurityException("URI caller differs from its authenticated process");
        return uid;
    }
    public static synchronized void start(Context context){
        if(service!=null)return;
        if(android.os.Process.myUid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("URI grants require the Android system owner");
        var lifecycle=new UriGrantsManagerService.Lifecycle(context);
        lifecycle.onStart();
        lifecycle.onBootPhase(SystemService.PHASE_SYSTEM_SERVICES_READY);
        var internal=LocalServices.getService(UriGrantsManagerInternal.class);
        internal.onSystemReady();
        service=(UriGrantsManagerService)HostUiAccess.get(lifecycle,UriGrantsManagerService.Lifecycle.class,"mService");
        local=internal;
        if(!HostLooper.addShutdownListener(context.getMainLooper(),HostUriGrants::flush))throw new IllegalStateException("Android system service owner has stopped");
    }
    public static synchronized UriGrantsManagerInternal owner(){
        if(local==null)throw new IllegalStateException("Android URI-grant service has not started");
        return local;
    }
    /** Called only by the private launcher lifecycle channel, after the
     * application's processes have retired and before its data is removed. */
    public static void removePackage(String name){
        if(name==null||name.equals("android"))throw new IllegalArgumentException("Invalid application package");
        owner().removeUriPermissionsForPackage(name,android.os.UserHandle.myUserId(),true,false);
        HostPackages.removeInstalled(name);
    }
    static int check(Uri uri,int sourceUser,int pid,int uid,int mode,boolean full,IBinder token)throws RemoteException{
        var target=remote();
        if(target!=null)return full?target.checkContentUriPermissionFull(uri,pid,uid,mode,sourceUser):target.checkUriPermission(uri,pid,uid,mode,sourceUser,token);
        if(local==null)return android.content.pm.PackageManager.PERMISSION_DENIED;
        int callingUid=caller();
        // Original ActivityManager grants its own system process access. This
        // check runs in the actual owner, never an application's local facade.
        if(pid==android.os.Process.myPid())return android.content.pm.PackageManager.PERMISSION_GRANTED;
        if(uid!=android.os.Process.ROOT_UID){
            String[] packages=HostPackages.service().getPackagesForUid(uid);
            if(packages==null||java.util.Arrays.stream(packages).allMatch(name->HostPackages.hostFilterAppAccess(null,name,callingUid,android.os.UserHandle.getUserId(uid))))return android.content.pm.PackageManager.PERMISSION_DENIED;
        }
        return owner().checkUriPermission(new GrantUri(sourceUser,uri,mode),uid,mode,full)
                ?android.content.pm.PackageManager.PERMISSION_GRANTED:android.content.pm.PackageManager.PERMISSION_DENIED;
    }
    static void grant(String target,Uri uri,int mode,int sourceUser,int targetUser)throws RemoteException{
        var manager=remote();if(manager!=null){manager.grantUriPermission(null,target,uri,mode,sourceUser);return;}
        int caller=caller();
        java.util.Objects.requireNonNull(target,"null target");
        targetUser=android.os.UserHandle.getUserId(caller);
        if(HostPackages.hostFilterAppAccess(null,target,caller,targetUser))return;
        com.android.internal.util.Preconditions.checkFlagsArgument(mode,Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION|Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        Intent intent=new Intent().setData(android.content.ContentProvider.maybeAddUserId(uri,sourceUser)).setFlags(mode);
        var needed=owner().checkGrantUriPermissionFromIntent(intent,caller,target,targetUser);
        owner().grantUriPermissionUncheckedFromIntent(needed,null);
    }
    static void revoke(String target,Uri uri,int mode,int sourceUser)throws RemoteException{
        var manager=remote();if(manager!=null){manager.revokeUriPermission(null,target,uri,mode,sourceUser);return;}
        int uid=caller();if(uri==null||!Intent.isAccessUriMode(mode))return;
        if(HostPackages.hostResolveContentProvider(null,uri.getAuthority(),android.content.pm.PackageManager.MATCH_DIRECT_BOOT_AWARE|android.content.pm.PackageManager.MATCH_DIRECT_BOOT_UNAWARE,sourceUser,uid)==null)return;
        owner().revokeUriPermission(target,uid,new GrantUri(sourceUser,uri,mode),mode);
    }
    /** Flush before the host retires package metadata and its private filesystem.
     * The original IoThread remains process-owned and can serve other services. */
    public static synchronized void flush(){
        if(service==null)return;
        Handler handler=(Handler)HostUiAccess.get(service,UriGrantsManagerService.class,"mH");
        handler.removeCallbacksAndMessages(null);
        Runnable write=()->HostUiAccess.call(HostUiAccess.method(UriGrantsManagerService.class,"writeGrantedUriPermissions"),service);
        if(handler.getLooper().getThread()==Thread.currentThread()){write.run();return;}
        var complete=new java.util.concurrent.CompletableFuture<Void>();
        if(!handler.post(()->{try{write.run();complete.complete(null);}catch(Throwable failure){complete.completeExceptionally(failure);}}))throw new IllegalStateException("Android URI persistence worker has stopped");
        complete.join();
    }
}
