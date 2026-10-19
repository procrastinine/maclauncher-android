package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.content.pm.*;
import android.os.*;
import java.io.IOException;
import java.util.*;

/** Actual remote process connections supplied by the launcher, with original AIDL clients. */
public final class HostRemoteServices {
    private static final HostProcessServices processes=new HostProcessServices();
    private static final Map<IBinder,Binding> bindings=new HashMap<>();
    private static HostBinderRpc.Peer systemPeer;
    private static HostProcessServices.Key platformOwner;
    private static Context publicationContext;
    /** Pending activation and connected services share the same client lifetime. */
    private static final class Binding implements IBinder.DeathRecipient {
        final IServiceConnection connection;
        final Set<IActivityManager> services=new HashSet<>();
        boolean closed;
        Binding(IServiceConnection connection){this.connection=connection;}
        @Override public void binderDied(){try{unbind(connection);}catch(RemoteException ignored){}}
        void connected(IActivityManager service,Intent intent,String type,long flags,String packageName,int user)throws RemoteException{
            synchronized(bindings){if(closed)return;}
            boolean accepted=service!=null&&service.bindService(null,null,intent,type,connection,flags,packageName,user)>0;
            boolean ended;
            synchronized(bindings){ended=closed;if(accepted&&!ended)services.add(service);}
            if(accepted&&ended)service.unbindService(connection);
            else if(!accepted&&!ended)connection.connected(intent.getComponent(),null,false);
        }
        void unavailable(ComponentName name){
            synchronized(bindings){if(closed)return;}
            try{connection.connected(name,null,true);}catch(RemoteException gone){binderDied();}
        }
    }
    private HostRemoteServices(){}
    public static void attach(HostBinderRpc.Connection connection,IBinder root)throws RemoteException{processes.attach(connection,root);}
    public static void stage(HostBinderRpc.Connection connection){processes.stage(connection);}
    public static void expect(HostBinderRpc.Peer peer){processes.expect(peer);}
    public static void expectIncoming(HostBinderRpc.Peer peer){processes.expectIncoming(peer);}
    public static void attach(HostBinderRpc.Connection connection,IBinder root,String connectionId)throws RemoteException{processes.attach(connection,root,connectionId);}
    public static void stage(HostBinderRpc.Connection connection,String connectionId){processes.stage(connection,connectionId);}
    public static void expect(HostBinderRpc.Peer peer,String connectionId){processes.expect(peer,connectionId);}
    /** The parent directory uses the same connection and death owner as APK peers. */
    public static void attachPlatform(String socket,HostBinderRpc.Peer peer)throws IOException,RemoteException{
        attachPlatform(socket,peer,null);
    }
    public static void attachPlatform(String socket,HostBinderRpc.Peer peer,String connectionId)throws IOException,RemoteException{
        processes.expect(peer,connectionId);
        var connection=HostBinderRpc.connect(socket,peer,4);
        try{
            var root=connection.root();processes.attach(connection,root,connectionId);
            var directory=IServiceManager.Stub.asInterface(root);
            HostServices.attachPlatform(directory);
            connection.closed().whenComplete((unused,failure)->HostServices.detachPlatform(directory));
            if(peer.uid()==android.os.Process.SYSTEM_UID&&peer.processName().equals("android.system")){
                Context context;synchronized(HostRemoteServices.class){systemPeer=peer;context=publicationContext;}
                if(context!=null)processes.publishDirectory(peer,context);
            }
        }catch(RemoteException|RuntimeException failure){processes.unavailable(peer,connectionId);connection.close();throw failure;}
    }
    /** Publish only after the application has installed its complete local services. */
    public static void publishPlatformServices(Context context){
        HostBinderRpc.Peer peer;synchronized(HostRemoteServices.class){publicationContext=Objects.requireNonNull(context);peer=systemPeer;}
        if(peer!=null)try{processes.publishDirectory(peer,context);}catch(RemoteException unavailable){throw new IllegalStateException("Android application services could not publish",unavailable);}
    }
    public static AutoCloseable listen(String socket,Context context)throws IOException {
        var directory=new HostServiceDirectory(context);
        try{var listener=HostBinderRpc.listen(socket,directory.root(),processes,4);return ()->{listener.close();directory.close();};}
        catch(Throwable failure){directory.close();throw failure;}
    }
    public static void attachAccounts(String socket,HostBinderRpc.Peer peer)throws IOException,RemoteException {
        accountsOwner(peer.packageName(),peer.processName());attachAccounts(socket,peer,null);
    }
    public static void accountsOwner(String packageName,String processName){
        HostAccounts.owner(packageName,processName);HostUriGrants.remoteOwner(packageName,processName);HostShortcuts.remoteOwner(packageName,processName);
    }
    public static void attachAccounts(String socket,HostBinderRpc.Peer peer,String connectionId)throws IOException,RemoteException {
        processes.expect(peer,connectionId);var connection=HostBinderRpc.connect(socket,peer,4);
        try{processes.attach(connection,connection.root(),connectionId);}
        catch(RemoteException|RuntimeException failure){processes.unavailable(peer,connectionId);connection.close();throw failure;}
    }
    /** Declarations use the existing lazy service owner; listing never connects. */
    public static void declare(HostProcessServices.Key owner,Map<String,String> declarations){
        declarations.forEach((name,descriptor)->HostServices.registerLazy(name,descriptor,()->{
            try{IBinder binder=systemService(owner,name);if(binder==null)throw new DeadObjectException();return ()->binder;}
            catch(RemoteException failure){throw failure.rethrowFromSystemServer();}
        }));
    }
    public static void platformOwner(HostProcessServices.Key owner){platformOwner=Objects.requireNonNull(owner);}
    /** PhoneWindow needs Settings during attach. Start its declared process
     * while the activity entry prepares graphics and APK records; acquisition
     * still waits on the same authenticated directory at the original call. */
    public static void prepareWindowServices(){
        var owner=platformOwner;if(owner!=null)processes.find(owner.packageName(),owner.processName());
    }
    static IBinder platformService(String name)throws RemoteException{var owner=platformOwner;return owner==null?null:systemService(owner,name);}
    public static void resolver(java.util.function.Consumer<HostProcessServices.Key> resolver){processes.resolver(resolver);}
    public static void resolver(java.util.function.BiConsumer<HostProcessServices.Key,String> resolver){processes.resolver(resolver);}
    public static void retirement(java.util.function.Consumer<HostProcessServices.Retirement> retirement){processes.retirement(retirement);}
    /** Terminal authority loss releases pending requests on the reader thread,
     * before the application's Looper can be asked to shut down. */
    public static void authorityClosed(){processes.close();}
    public static void unavailable(String packageName,String processName){processes.unavailable(packageName,processName);}
    public static void unavailable(HostProcessServices.Key process,String requestId){processes.unavailable(process,requestId);}
    public static void unavailable(HostBinderRpc.Peer peer){processes.unavailable(peer);}
    public static void unavailable(HostBinderRpc.Peer peer,String connectionId){processes.unavailable(peer,connectionId);}
    private static HostProcessServices.Process owner(IIntentSender sender)throws RemoteException{
        if(sender.asBinder() instanceof Binder)return null;
        var peer=HostBinderRpc.ownerOf(sender.asBinder());if(peer==null)return null;
        var process=processes.find(peer);
        if(process==null)throw new DeadObjectException();
        return process;
    }
    static IActivityManager intentManager(IIntentSender sender)throws RemoteException{var process=owner(sender);if(process==null)return null;IBinder binder=process.service(Context.ACTIVITY_SERVICE);if(binder==null)throw new DeadObjectException();return IActivityManager.Stub.asInterface(binder);}
    static IActivityTaskManager intentTasks(IIntentSender sender)throws RemoteException{var process=owner(sender);if(process==null)return null;IBinder binder=process.service(Context.ACTIVITY_TASK_SERVICE);if(binder==null)throw new DeadObjectException();return IActivityTaskManager.Stub.asInterface(binder);}
    static java.util.concurrent.CompletionStage<IActivityTaskManager> activityTasksAsync(ActivityInfo info){
        String name=info.processName==null?info.applicationInfo.processName:info.processName;
        var process=processes.find(info.packageName,name);if(process==null)return null;
        return process.serviceAsync(Context.ACTIVITY_TASK_SERVICE).thenApply(IActivityTaskManager.Stub::asInterface);
    }
    static IActivityManager resolve(ServiceInfo info)throws RemoteException{var process=processes.find(info);if(process==null)return null;IBinder binder=process.service(Context.ACTIVITY_SERVICE);return binder==null?null:IActivityManager.Stub.asInterface(binder);}
    /** Manifest receivers use their declared process and the original broadcast protocol. */
    public static boolean broadcast(ActivityInfo info,IApplicationThread caller,Intent intent,String type,IIntentReceiver result,int code,String data,Bundle extras,boolean ordered,int user)throws RemoteException{
        var process=processes.find(info.packageName,info.processName==null?info.packageName:info.processName);if(process==null)return false;
        IBinder binder=process.service(Context.ACTIVITY_SERVICE);if(binder==null)return false;
        IActivityManager.Stub.asInterface(binder).broadcastIntent(caller,intent,type,result,code,data,extras,null,-1,null,ordered,false,user);return true;
    }
    static IBinder systemService(HostProcessServices.Key owner,String name)throws RemoteException{var process=processes.find(owner.packageName(),owner.processName());return process==null?null:process.service(name);}
    static ContentProviderHolder provider(IApplicationThread caller,String packageName,ProviderInfo info,String authority,int user,boolean stable)throws RemoteException{
        var process=HostPlatformPackages.owns(info)?processes.find("android","android.system"):processes.find(info.packageName,info.processName==null?info.packageName:info.processName);if(process==null)return null;
        try{
            var manager=IActivityManager.Stub.asInterface(process.service(Context.ACTIVITY_SERVICE));
            return manager==null?null:manager.getContentProvider(caller,packageName,authority,user,stable);
        }catch(DeadObjectException unavailable){
            // This Binder belongs to the provider application, not to the local
            // ActivityManager. Original AMS returns no holder when that process
            // fails to launch or dies before acquisition; forwarding its death
            // would make ActivityThread report that the system server died.
            android.util.Log.w("ActivityManager","Content provider process unavailable: "+info.packageName+"/"+info.name+" ("+unavailable.getClass().getName()+")");
            return null;
        }
    }
    static IActivityManager providerManager(IBinder reference)throws RemoteException{
        if(reference==null||reference instanceof Binder)return null;
        var peer=HostBinderRpc.ownerOf(reference);if(peer==null)return null;
        var process=processes.find(peer);if(process==null)throw new DeadObjectException();
        var manager=IActivityManager.Stub.asInterface(process.service(Context.ACTIVITY_SERVICE));if(manager==null)throw new DeadObjectException();return manager;
    }
    static ComponentName start(Context context,ServiceInfo info,Intent intent)throws RemoteException{var service=resolve(info);return service==null?null:service.startService(null,intent,intent.resolveTypeIfNeeded(context.getContentResolver()),false,HostContext.hostPackageName(),null,context.getUserId());}
    static boolean stop(Context context,ServiceInfo info,Intent intent)throws RemoteException{var service=resolve(info);return service!=null&&service.stopService(null,intent,intent.resolveTypeIfNeeded(context.getContentResolver()),context.getUserId())>0;}
    static boolean bind(Context context,ServiceInfo info,Intent intent,IServiceConnection connection,long flags)throws RemoteException{
        var process=processes.find(info);if(process==null)return false;
        Objects.requireNonNull(connection);IBinder client=connection.asBinder();Binding binding;
        synchronized(bindings){
            binding=bindings.get(client);
            if(binding==null){binding=new Binding(connection);client.linkToDeath(binding,0);bindings.put(client,binding);}
        }
        Binding owned=binding;Intent selected=new Intent(intent);String type=selected.resolveTypeIfNeeded(context.getContentResolver());
        String packageName=HostContext.hostPackageName();int user=context.getUserId();
        process.serviceAsync(Context.ACTIVITY_SERVICE).whenComplete((binder,failure)->{
            if(failure!=null){owned.unavailable(selected.getComponent());return;}
            try{owned.connected(IActivityManager.Stub.asInterface(binder),selected,type,flags,packageName,user);}
            catch(RemoteException|RuntimeException connectionFailure){android.util.Log.w("ActivityManager","Android service could not connect",connectionFailure);owned.unavailable(selected.getComponent());}
        });
        return true;
    }
    static boolean unbind(IServiceConnection connection)throws RemoteException{
        Binding owner;Set<IActivityManager> services;IBinder client=connection.asBinder();
        synchronized(bindings){owner=bindings.remove(client);if(owner==null)return false;owner.closed=true;services=Set.copyOf(owner.services);owner.services.clear();}
        try{client.unlinkToDeath(owner,0);}catch(NoSuchElementException gone){}
        for(var service:services)try{service.unbindService(connection);}catch(DeadObjectException gone){}
        return true;
    }
    public static void shutdown(){synchronized(HostRemoteServices.class){systemPeer=null;publicationContext=null;platformOwner=null;}List<Binding> snapshot;synchronized(bindings){snapshot=List.copyOf(bindings.values());}for(Binding binding:snapshot)try{unbind(binding.connection);}catch(RemoteException gone){}processes.close();}
}
