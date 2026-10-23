package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.os.*;
import com.android.internal.os.IResultReceiver;
import com.android.server.am.HostPendingIntentInjector;
import java.util.*;

/** Local process and intent-token endpoint beneath Android's original service clients. */
public final class HostActivityService extends IActivityManager.Default implements AutoCloseable {
    private Context context;
    private HostPendingIntentInjector pendingIntents;
    private HostProviderConnections providers;
    private com.android.server.am.HostUidObservers uidObservers;
    private final IActivityManager localClient;
    private volatile boolean closed;
    /** The original ContextImpl queries permissions while creating its first
     * Context. Publish the existing process endpoint before that construction. */
    HostActivityService(){
        localClient=(IActivityManager)HostServiceClients.local(this,"android.app.IActivityManager");
        HostServices.register(Context.ACTIVITY_SERVICE,this,"android.app.IActivityManager");
        // LoadedApk may query the shared profile before this process has its
        // application Context. Adopt the local owner in the original client's
        // cache too; service-directory precedence cannot replace a cached hop.
        Object singleton=activitySingleton();
        synchronized(singleton){HostUiAccess.set(singleton,android.util.Singleton.class,"mInstance",localClient);}
    }
    HostActivityService(Context context){this();attach(context);}
    /** Context-dependent original owners start after the real Context exists. */
    synchronized void attach(Context context){
        if(closed||this.context!=null)throw new IllegalStateException("Activity service is already attached or retired");
        this.context=Objects.requireNonNull(context);
        try{
            pendingIntents=new HostPendingIntentInjector(context);
            providers=new HostProviderConnections(context);
            uidObservers=new com.android.server.am.HostUidObservers(context);
        }catch(RuntimeException|Error failure){
            try{close();}catch(RuntimeException|Error cleanup){failure.addSuppressed(cleanup);}
            throw failure;
        }
    }
    private static Object activitySingleton(){return HostUiAccess.get(null,ActivityManager.class,"IActivityManagerSingleton");}
    private void retireClient(){Object singleton=activitySingleton();synchronized(singleton){if(HostUiAccess.get(singleton,android.util.Singleton.class,"mInstance")==localClient)HostUiAccess.set(singleton,android.util.Singleton.class,"mInstance",null);}}
    @Override public ContentProviderHolder getContentProvider(IApplicationThread caller,String packageName,String authority,int user,boolean stable)throws RemoteException{
        if(closed||user!=context.getUserId())return null;
        var peer=HostBinderRpc.callingPeer();
        if(peer==null?!Objects.equals(packageName,context.getPackageName()):!Objects.equals(packageName,peer.packageName())||peer.pid()!=Binder.getCallingPid()||peer.uid()!=Binder.getCallingUid())throw new SecurityException("Provider caller does not match the calling process");
        var info=HostPackages.service().resolveContentProvider(authority,android.content.pm.PackageManager.GET_META_DATA,user);
        if(info==null||!info.enabled)return null;
        boolean local=HostPackages.owns(info);
        if(!local){if(peer!=null)throw new SecurityException("Provider belongs to another process");return HostRemoteServices.provider(caller,packageName,info,authority,user,stable);}
        if(peer!=null){var holder=providers.acquire(caller,peer,info,authority,stable);HostPlatformPackages.request(context,info);return holder;}
        if(HostPlatformPackages.owns(info)){
            HostPlatformPackages.demand(context,info);
            var endpoint=ActivityThread.currentActivityThread().acquireExistingProvider(context,authority,user,stable);
            // A provider can query Settings during its own SQLite startup.
            // Its unfinished attachment is not a second local installation.
            if(endpoint==null)return null;
            var holder=new ContentProviderHolder(info);holder.provider=endpoint;holder.mLocal=true;holder.noReleaseNeeded=true;return holder;
        }
        var holder=new ContentProviderHolder(info);holder.mLocal=true;holder.noReleaseNeeded=true;return holder;
    }
    @Override public void publishContentProviders(IApplicationThread caller,List<ContentProviderHolder> providers){
        if(HostBinderRpc.callingPeer()!=null)throw new SecurityException("Provider publication belongs to its application process");
        for(var holder:providers)if(holder.provider==null||!HostPackages.owns(holder.info))throw new SecurityException("Content provider belongs to another application");
        HostGoogleProviders.published(context,providers);
        this.providers.published(providers);
    }
    @Override public boolean refContentProvider(IBinder connection,int stable,int unstable)throws RemoteException{var owner=HostRemoteServices.providerManager(connection);return owner==null?providers.ref(connection,stable,unstable):owner.refContentProvider(connection,stable,unstable);}
    @Override public void removeContentProvider(IBinder connection,boolean stable)throws RemoteException{var owner=HostRemoteServices.providerManager(connection);if(owner==null)providers.remove(connection,stable);else owner.removeContentProvider(connection,stable);}
    @Override public void unstableProviderDied(IBinder connection)throws RemoteException{var owner=HostRemoteServices.providerManager(connection);if(owner==null)providers.died(connection);else owner.unstableProviderDied(connection);}
    @Override public void appNotRespondingViaProvider(IBinder connection){System.err.println("[android-host] Content provider is not responding");}
    @Override public IIntentSender getIntentSender(int type,String pkg,IBinder token,String who,int request,Intent[] intents,String[] types,int flags,Bundle options,int user){return getIntentSenderWithFeature(type,pkg,null,token,who,request,intents,types,flags,options,user);}
    @Override public IIntentSender getIntentSenderWithFeature(int type,String pkg,String feature,IBinder token,String who,int request,Intent[] intents,String[] types,int flags,Bundle options,int user){
        var peer=HostBinderRpc.callingPeer();int uid=peer==null?android.os.Process.myUid():peer.uid();String creator=peer==null?context.getPackageName():peer.packageName();
        if(!Objects.equals(pkg,creator)||user!=context.getUserId())throw new SecurityException("Intent creator does not match the calling process");
        if(closed)return null;
        return pendingIntents.create(type,pkg,feature,uid,user,token,who,request,intents,types,flags,options);
    }
    @Override public void cancelIntentSender(IIntentSender sender)throws RemoteException{var remote=HostRemoteServices.intentManager(sender);if(remote==null)pendingIntents.cancel(sender);else remote.cancelIntentSender(sender);}
    @Override public boolean registerIntentSenderCancelListenerEx(IIntentSender sender,IResultReceiver listener)throws RemoteException{var remote=HostRemoteServices.intentManager(sender);return remote==null?pendingIntents.register(sender,listener):remote.registerIntentSenderCancelListenerEx(sender,listener);}
    @Override public void unregisterIntentSenderCancelListener(IIntentSender sender,IResultReceiver listener)throws RemoteException{var remote=HostRemoteServices.intentManager(sender);if(remote==null)pendingIntents.unregister(sender,listener);else remote.unregisterIntentSenderCancelListener(sender,listener);}
    @Override public ActivityManager.PendingIntentInfo getInfoForIntentSender(IIntentSender sender)throws RemoteException{var remote=HostRemoteServices.intentManager(sender);return remote==null?HostPendingIntentInjector.info(sender):remote.getInfoForIntentSender(sender);}
    @Override public Intent getIntentForIntentSender(IIntentSender sender)throws RemoteException{var remote=HostRemoteServices.intentManager(sender);return remote==null?HostPendingIntentInjector.intent(sender):remote.getIntentForIntentSender(sender);}
    @Override public boolean isIntentSenderAnActivity(IIntentSender sender)throws RemoteException{var remote=HostRemoteServices.intentManager(sender);return remote==null?HostPendingIntentInjector.activity(sender):remote.isIntentSenderAnActivity(sender);}
    @Override public int sendIntentSender(IApplicationThread caller,IIntentSender sender,IBinder allowlist,int code,Intent intent,String resolvedType,IIntentReceiver finished,String permission,Bundle options)throws RemoteException{return HostActivityTasks.windowRequest(options,windowOptions->{var remote=HostRemoteServices.intentManager(sender);return remote==null?HostPendingIntentInjector.send(caller,sender,allowlist,code,intent,resolvedType,finished,permission,options):remote.sendIntentSender(caller,sender,allowlist,code,intent,resolvedType,finished,permission,windowOptions);});}
    @Override public int checkPermission(String permission,int pid,int uid){return HostPermissions.check(permission,uid,Context.DEVICE_ID_DEFAULT);}
    @Override public int checkPermissionForDevice(String permission,int pid,int uid,int device){return HostPermissions.check(permission,uid,device);}
    @Override public android.content.res.Configuration getConfiguration(){return HostApplication.systemConfiguration();}
    @Override public void getMemoryInfo(ActivityManager.MemoryInfo result){HostActivityManager.getMemoryInfo(result);}
    @Override public Debug.MemoryInfo[] getProcessMemoryInfo(int[] pids){return HostActivityManager.getProcessMemoryInfo(pids);}
    @Override public long[] getProcessPss(int[] pids){return HostActivityManager.getProcessPss(pids);}
    @Override public void getMyMemoryState(ActivityManager.RunningAppProcessInfo result){HostActivityManager.getMyMemoryState(result);}
    @Override public android.content.pm.ParceledListSlice<ApplicationExitInfo> getHistoricalProcessExitReasons(String pkg,int pid,int max,int user){return new android.content.pm.ParceledListSlice<>(user==context.getUserId()?HostActivityManager.getHistoricalProcessExitReasons(context,pkg,pid,max):List.of());}
    private boolean own(String pkg,int user){return Objects.equals(pkg,context.getPackageName())&&user==context.getUserId();}
    // USER_ALL and current-user selectors refer to this installation's single
    // hosted Android user. They do not change the calling package's authority.
    private boolean broadcastUser(int user){return user==context.getUserId()||user==UserHandle.USER_ALL||user==UserHandle.USER_CURRENT||user==UserHandle.USER_CURRENT_OR_SELF;}
    // Original ActivityManager distinguishes absent (0), denied (-1) and
    // resolved (1) services. In particular, optional bindings are not denials.
    private int serviceCaller(String pkg,int user,Intent intent){
        if(user!=context.getUserId())return -1;var peer=HostBinderRpc.callingPeer();
        if(peer==null){if(!own(pkg,user))return -1;}else if(!Objects.equals(peer.packageName(),pkg)||peer.pid()!=Binder.getCallingPid()||peer.uid()!=Binder.getCallingUid())return -1;
        var resolved=context.getPackageManager().resolveService(intent,android.content.pm.PackageManager.GET_META_DATA);if(resolved==null||resolved.serviceInfo==null)return 0;
        var info=resolved.serviceInfo;if(peer!=null&&(!info.packageName.equals(context.getPackageName())||!Objects.equals(info.processName,HostContext.hostProcessName())))return -1;
        try{return HostPackages.permitsService(pkg,info)?1:-1;}catch(Exception failure){throw new SecurityException("Android service permission could not be verified",failure);}
    }
    @Override public ComponentName startService(IApplicationThread caller,Intent intent,String type,boolean foreground,String pkg,String feature,int user){int access=serviceCaller(pkg,user,intent);if(access<0)throw new SecurityException("Android service access denied");return access==0?null:HostLocalServices.start(context,intent);}
    @Override public int stopService(IApplicationThread caller,Intent intent,String type,int user){var peer=HostBinderRpc.callingPeer();String pkg=peer==null?context.getPackageName():peer.packageName();int access=serviceCaller(pkg,user,intent);return access<=0?access:HostLocalServices.stop(context,intent)?1:0;}
    @Override public int bindService(IApplicationThread caller,IBinder token,Intent intent,String type,IServiceConnection connection,long flags,String pkg,int user){return bindServiceInstance(caller,token,intent,type,connection,flags,null,pkg,user);}
    @Override public int bindServiceInstance(IApplicationThread caller,IBinder token,Intent intent,String type,IServiceConnection connection,long flags,String instance,String pkg,int user){int access=serviceCaller(pkg,user,intent);if(access<=0)return access;if(instance!=null){System.err.println("[android-host] Isolated service process could not be found: "+instance);return 0;}return HostLocalServices.bind(context,intent,connection,flags)?1:0;}
    @Override public boolean unbindService(IServiceConnection connection){return HostLocalServices.unbind(connection);}
    @Override public Intent registerReceiver(IApplicationThread caller,String pkg,IIntentReceiver receiver,IntentFilter filter,String permission,int user,int flags){return registerReceiverWithFeature(caller,pkg,null,null,receiver,filter,permission,user,flags);}
    @Override public Intent registerReceiverWithFeature(IApplicationThread caller,String pkg,String feature,String id,IIntentReceiver receiver,IntentFilter filter,String permission,int user,int flags){if(!Objects.equals(pkg,context.getPackageName())||!broadcastUser(user))throw new SecurityException("Receiver belongs to another application");return maclauncher.android.bridge.LocalBroadcasts.register(caller,receiver,filter,permission,flags);}
    @Override public void unregisterReceiver(IIntentReceiver receiver){maclauncher.android.bridge.LocalBroadcasts.unregister(receiver);}
    @Override public int broadcastIntent(IApplicationThread caller,Intent intent,String type,IIntentReceiver result,int code,String data,Bundle extras,String[] permissions,int appOp,Bundle options,boolean ordered,boolean sticky,int user){return broadcastIntentWithFeature(caller,null,intent,type,result,code,data,extras,permissions,null,null,appOp,options,ordered,sticky,user);}
    @Override public int broadcastIntentWithFeature(IApplicationThread caller,String feature,Intent intent,String type,IIntentReceiver result,int code,String data,Bundle extras,String[] permissions,String[] excludedPermissions,String[] excludedPackages,int appOp,Bundle options,boolean ordered,boolean sticky,int user){return broadcastUser(user)?maclauncher.android.bridge.LocalBroadcasts.send(context,caller,intent,type,result,code,data,extras,permissions,excludedPermissions,excludedPackages,ordered,sticky,context.getUserId()):0;}
    @Override public void unbroadcastIntent(IApplicationThread caller,Intent intent,int user){if(broadcastUser(user))maclauncher.android.bridge.LocalBroadcasts.removeSticky(intent);}
    @Override public void finishReceiver(IBinder who,int code,String data,Bundle extras,boolean abort,int flags){maclauncher.android.bridge.LocalBroadcasts.finish(who,code,data,extras,abort);}
    @Override public int checkUriPermission(android.net.Uri uri,int pid,int uid,int mode,int user,IBinder token)throws RemoteException{return HostUriGrants.check(uri,user,pid,uid,mode,false,token);}
    @Override public int checkContentUriPermissionFull(android.net.Uri uri,int pid,int uid,int mode,int user)throws RemoteException{return HostUriGrants.check(uri,user,pid,uid,mode,true,null);}
    @Override public int[] checkUriPermissions(List<android.net.Uri> uris,int pid,int uid,int mode,int user,IBinder token)throws RemoteException{int[] results=new int[uris.size()];for(int i=0;i<uris.size();i++){var uri=uris.get(i);results[i]=checkUriPermission(ContentProvider.getUriWithoutUserId(uri),pid,uid,mode,ContentProvider.getUserIdFromUri(uri,user),token);}return results;}
    @Override public void grantUriPermission(IApplicationThread caller,String target,android.net.Uri uri,int mode,int user)throws RemoteException{HostUriGrants.grant(target,Objects.requireNonNull(uri),mode,user,context.getUserId());}
    @Override public void revokeUriPermission(IApplicationThread caller,String target,android.net.Uri uri,int mode,int user)throws RemoteException{HostUriGrants.revoke(target,uri,mode,user);}
    @Override public boolean isUidActive(int uid,String pkg){return uid==android.os.Process.myUid();}
    @Override public int getUidProcessState(int uid,String pkg){return HostActivityManager.hostUidProcessState(null,uid);}
    @Override public void registerUidObserver(IUidObserver observer,int which,int cutpoint,String pkg){uidObservers.register(observer,which,cutpoint,pkg,null);}
    @Override public IBinder registerUidObserverForUids(IUidObserver observer,int which,int cutpoint,String pkg,int[] uids){return uidObservers.register(observer,which,cutpoint,pkg,uids);}
    @Override public void unregisterUidObserver(IUidObserver observer){uidObservers.unregister(observer);}
    @Override public void addUidToObserver(IBinder token,String pkg,int uid){uidObservers.add(token,pkg,uid);}
    @Override public void removeUidFromObserver(IBinder token,String pkg,int uid){uidObservers.remove(token,pkg,uid);}
    @Override public void registerUidFrozenStateChangedCallback(IUidFrozenStateChangedCallback callback){uidObservers.registerFrozen(callback);}
    @Override public void unregisterUidFrozenStateChangedCallback(IUidFrozenStateChangedCallback callback){uidObservers.unregisterFrozen(callback);}
    @Override public int[] getUidFrozenState(int[] uids){return uidObservers.frozen(uids);}
    @Override public int getCurrentUserId(){return context.getUserId();}
    @Override public boolean stopServiceToken(ComponentName component,IBinder token,int id){return HostLocalServices.stopToken(component,token,id);}
    @Override public void setServiceForeground(ComponentName component,IBinder token,int id,Notification notification,int flags,int type){HostLocalServices.foreground(component,token,id,notification,flags,type);}
    @Override public int getForegroundServiceType(ComponentName component,IBinder token){return HostLocalServices.foregroundType(component,token);}
    @Override public List<ActivityManager.RunningServiceInfo> getServices(int max,int flags){var peer=HostBinderRpc.callingPeer();return closed||peer!=null&&peer.uid()!=android.os.Process.myUid()?new ArrayList<>():HostLocalServices.running(max);}
    @Override public List<ActivityManager.RunningAppProcessInfo> getRunningAppProcesses(){return HostActivityManager.getRunningAppProcesses(context);}
    @Override public void close(){
        synchronized(this){if(closed)return;closed=true;}
        try{
            if(uidObservers!=null)uidObservers.close();
            if(providers!=null)providers.close();
            if(pendingIntents!=null)pendingIntents.close();
        }finally{
            HostServices.unregister(Context.ACTIVITY_SERVICE,this,"android.app.IActivityManager");
            // Earlier service listeners still unregister their original receivers
            // during Looper retirement. A bootstrap failure before Context attach
            // has no such listeners and retires its cached client immediately.
            if(context==null||!HostLooper.addShutdownListener(context.getMainLooper(),this::retireClient))retireClient();
        }
    }
}
