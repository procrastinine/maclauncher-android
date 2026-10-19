package maclauncher.android.framework;

import android.accounts.*;
import android.app.AppOpsManager;
import android.content.*;
import android.os.*;
import android.util.SparseArray;
import com.android.server.accounts.AccountManagerService;
import com.android.server.accounts.HostAccountInjector;
import java.nio.file.*;
import java.util.*;

/** Owns the supplied Android account service and its private process lifetime. */
public final class HostAccounts implements AutoCloseable {
    private static HostAccounts current;
    private static HostProcessServices.Key remoteOwner;
    /** The supervisor selects one original account-service owner for its APK group. */
    public static void owner(String packageName,String processName){remoteOwner=packageName==null?null:new HostProcessServices.Key(packageName,processName);}
    private final Context context;
    private final AccountManagerService service;
    private final IAccountManager endpoint;
    private final Handler handler;
    private boolean closed;
    private HostAccounts(Context parent)throws Exception {
        context=parent.createAttributionContext(parent.getAttributionTag());
        int user=context.getUserId();
        for(var directory:new java.io.File[]{Environment.getDataSystemDeDirectory(user),Environment.getDataSystemCeDirectory(user),Environment.getUserSystemDirectory(user)})
            Files.createDirectories(HostFilesystem.file(directory).toPath());
        var injector=new HostAccountInjector(context);
        AccountManagerService created=null;
        try{
            created=new AccountManagerService(injector);
            service=created;
            endpoint=HostAccountAccess.endpoint(service);
            handler=(Handler)HostUiAccess.get(service,AccountManagerService.class,"mHandler");
            HostUiAccess.call(HostUiAccess.method(AccountManagerService.class,"onUnlockUser",int.class),service,user);
        }catch(Throwable failure){
            try{HostApplication.releaseReceivers(context);}catch(Throwable cleanup){failure.addSuppressed(cleanup);}
            injector.closeWorker();
            throw failure;
        }
    }
    public static void prepare(Context context){
        HostServices.registerLazy(Context.ACCOUNT_SERVICE,"android.accounts.IAccountManager",()->{
            if(remoteOwner!=null)try{
                IBinder service=HostRemoteServices.systemService(remoteOwner,Context.ACCOUNT_SERVICE);
                if(service==null)throw new DeadObjectException();
                return IAccountManager.Stub.asInterface(service);
            }catch(RemoteException unavailable){throw unavailable.rethrowFromSystemServer();}
            try{return install(context).endpoint;}catch(RuntimeException|Error failure){throw failure;}catch(Exception failure){throw new IllegalStateException("Android account service could not start",failure);}
        });
    }
    public static synchronized HostAccounts install(Context context)throws Exception {
        if(current!=null){if(current.closed)throw new IllegalStateException("Android account service is closed");return current;}
        HostAccounts owner=new HostAccounts(context);
        if(!HostLooper.addShutdownListener(context.getMainLooper(),owner::close)){owner.close();throw new IllegalStateException("Android account owner has stopped");}
        return current=owner;
    }
    public static void selected(Context context,HostBinderRpc.Peer peer,Account account)throws Exception {
        if(account==null)return;
        var original=install(context).service;
        if(!Arrays.asList(original.getAccountsAsUser(null,context.getUserId(),context.getPackageName())).contains(account))throw new IllegalArgumentException("The selected Android account is unavailable");
        original.setAccountVisibility(account,peer.packageName(),AccountManager.VISIBILITY_VISIBLE);
    }
    public static void cancel(IAccountManagerResponse response){
        HostAccounts owner=current;if(owner==null)return;
        var sessions=(Map<?,?>)HostUiAccess.get(owner.service,AccountManagerService.class,"mSessions");
        List<?> snapshot;synchronized(sessions){snapshot=new ArrayList<>(sessions.values());}
        var type=HostUiAccess.type("com.android.server.accounts.AccountManagerService$Session");
        for(var session:snapshot){var pending=(IAccountManagerResponse)HostUiAccess.get(session,type,"mResponse");
            if(pending!=null&&Objects.equals(pending.asBinder(),response.asBinder()))try{((IAccountAuthenticatorResponse)session).onError(AccountManager.ERROR_CODE_CANCELED,"Account operation cancelled");}catch(RemoteException ignored){}
        }
    }
    public static synchronized void shutdown(){HostServices.unregisterLazy(Context.ACCOUNT_SERVICE);if(current!=null)current.close();}
    @Override @SuppressWarnings("unchecked") public synchronized void close(){
        if(closed)return;closed=true;
        HostServices.unregister(Context.ACCOUNT_SERVICE,endpoint,"android.accounts.IAccountManager");
        Throwable failure=null;
        try{
            var sessions=(Map<String,?>)HostUiAccess.get(service,AccountManagerService.class,"mSessions");
            List<?> snapshot;synchronized(sessions){snapshot=new ArrayList<>(sessions.values());}
            var sessionClass=HostUiAccess.type("com.android.server.accounts.AccountManagerService$Session");
            for(Object session:snapshot)HostUiAccess.call(HostUiAccess.method(sessionClass,"close"),session);
            ((com.android.internal.content.PackageMonitor)HostUiAccess.get(service,AccountManagerService.class,"mPackageMonitor")).unregister();
            var appOps=(AppOpsManager)HostUiAccess.get(service,AccountManagerService.class,"mAppOpsManager");
            var watches=(Map<AppOpsManager.OnOpChangedListener,?>)HostUiAccess.get(appOps,AppOpsManager.class,"mModeWatchers");
            List<AppOpsManager.OnOpChangedListener> callbacks;synchronized(watches){callbacks=new ArrayList<>(watches.keySet());}
            for(var callback:callbacks)appOps.stopWatchingMode(callback);
            HostApplication.releaseReceivers(context);
        }catch(Throwable error){failure=error;}
        handler.removeCallbacksAndMessages(null);handler.getLooper().quitSafely();
        if(handler.getLooper().getThread()!=Thread.currentThread())try{handler.getLooper().getThread().join();}catch(InterruptedException interrupted){Thread.currentThread().interrupt();if(failure==null)failure=interrupted;else failure.addSuppressed(interrupted);}
        var users=(SparseArray<?>)HostUiAccess.get(service,AccountManagerService.class,"mUsers");
        synchronized(users){for(int i=0;i<users.size();i++){
            Object user=users.valueAt(i);var type=user.getClass();Object lock=HostUiAccess.get(user,type,"dbLock");
            synchronized(lock){try{((AutoCloseable)HostUiAccess.get(user,type,"accountsDb")).close();}catch(Exception error){if(failure==null)failure=error;else failure.addSuppressed(error);}}
        }users.clear();}
        if(failure!=null)HostUiAccess.raise(failure);
    }
}
