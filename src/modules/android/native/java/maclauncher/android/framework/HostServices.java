package maclauncher.android.framework;

import android.os.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Supplier;

/** Process-local service-manager endpoint. The full AOSP service clients remain
 * responsible for their overloads, validation, caching and API behavior. */
public final class HostServices extends IServiceManager.Stub {
    private record Registration(IBinder binder,boolean published) {}
    private static final class State {
        final Map<String,Registration> services=new LinkedHashMap<>();
        final Map<String,Pending> lazy=new LinkedHashMap<>();
        final Map<String,Map<IClientCallback,IBinder.DeathRecipient>> clientCallbacks=new HashMap<>();
        volatile List<IServiceManager> platforms=List.of();
        final List<HostServices> directories=new ArrayList<>();
        HostBinderRpc.Peer peer;
        boolean retired;
    }
    private static final State GLOBAL=new State();
    private static final Map<HostBinderRpc.Peer,HostServices> scopes=new HashMap<>();
    private static final Map<String,List<java.util.function.Consumer<ScopedService>>> observers=new HashMap<>();
    record ScopedService(HostBinderRpc.Peer peer,String name,IBinder binder){}
    private final State state;
    private static final HostServices INSTANCE=new HostServices(false);
    private static final HostServices PUBLISHED=new HostServices(true);
    private final boolean publishedOnly;
    private final Map<String,List<IServiceCallback>> callbacks=new HashMap<>();
    private final Set<String> missing=new HashSet<>();
    private HostServices(boolean publishedOnly){this(publishedOnly,GLOBAL);}
    private HostServices(boolean publishedOnly,State state){this.publishedOnly=publishedOnly;this.state=state;state.directories.add(this);}
    /** Native composition services retain their authenticated process namespace.
     * Shared profile services remain the same original parent directory. */
    static IServiceManager scopedManager(HostBinderRpc.Peer peer){
        synchronized(GLOBAL){return scopes.computeIfAbsent(peer,owner->{State state=new State();state.peer=owner;state.platforms=List.of(PUBLISHED);return new HostServices(false,state);});}
    }
    static AutoCloseable observeScopes(String name,java.util.function.Consumer<ScopedService> observer){
        synchronized(GLOBAL){
            observers.computeIfAbsent(name,key->new ArrayList<>()).add(observer);
            var registration=GLOBAL.services.get(name);if(registration!=null)observer.accept(new ScopedService(null,name,registration.binder()));
            for(var entry:scopes.entrySet()){var value=entry.getValue().state.services.get(name);if(value!=null)observer.accept(new ScopedService(entry.getKey(),name,value.binder()));}
        }
        return ()->{synchronized(GLOBAL){var listeners=observers.get(name);if(listeners!=null){listeners.remove(observer);if(listeners.isEmpty())observers.remove(name);}}};
    }
    private void notifyScopes(String name,IBinder binder){
        synchronized(GLOBAL){for(var observer:List.copyOf(observers.getOrDefault(name,List.of())))observer.accept(new ScopedService(state.peer,name,binder));}
    }
    static void retirePeer(HostBinderRpc.Peer peer){
        HostPackages.retirePeer(peer);
        synchronized(GLOBAL){var manager=scopes.remove(peer);if(manager==null)return;manager.state.retired=true;
            for(String name:List.copyOf(manager.state.services.keySet())){manager.state.services.remove(name);manager.removeClientCallbacks(name);manager.notifyScopes(name,null);}manager.callbacks.clear();}
    }
    private static volatile boolean platformAuthorization;
    static void enablePlatformAuthorization(){platformAuthorization=true;}
    private static boolean remotePolicy(){return platformAuthorization&&HostBinderRpc.callingPeer()!=null;}
    private static boolean canFind(String name){return !remotePolicy()||canFindNative(name);}
    private static void requireFind(String name){if(!canFind(name))throw new SecurityException("Service access denied");}
    private static void requireList(){if(remotePolicy()&&!canListNative())throw new SecurityException("Service listing denied");}
    private static native boolean canFindNative(String name);
    private static native boolean canAddNative(String name);
    private static native boolean canListNative();
    /** The authenticated profile connection owns this directory's lifetime. */
    public static void attachPlatform(IServiceManager directory){
        Objects.requireNonNull(directory);
        if(directory==INSTANCE||directory.asBinder()==INSTANCE.asBinder())throw new IllegalArgumentException("A service directory cannot delegate to itself");
        synchronized(GLOBAL){if(GLOBAL.platforms.stream().noneMatch(parent->parent.asBinder().equals(directory.asBinder()))){var parents=new ArrayList<>(GLOBAL.platforms);parents.add(directory);GLOBAL.platforms=List.copyOf(parents);}}
    }
    public static void detachPlatform(IServiceManager expected){synchronized(GLOBAL){GLOBAL.platforms=GLOBAL.platforms.stream().filter(parent->parent!=expected).toList();}}
    /** Query the authenticated shared owner explicitly, without shadowing a
     * process's own ActivityManager or other local service binding. */
    public static IBinder platformService(String name)throws RemoteException{
        for(var directory:GLOBAL.platforms){var service=directory.checkService(name);if(service!=null)return service;}return HostRemoteServices.platformService(name);
    }
    private IServiceManager parent(String name)throws RemoteException{if(isLocal(name))return null;for(var directory:parents())if(directory.checkService(name)!=null)return directory;return null;}
    private List<IServiceManager> parents(){return publishedOnly?List.of():state.platforms;}
    private boolean visible(Registration registration){return registration!=null&&(!publishedOnly||registration.published());}
    private boolean visible(Pending pending){return pending!=null&&(!publishedOnly||pending.published);}
    private boolean isLocal(String name){synchronized(GLOBAL){return visible(state.services.get(name))||visible(state.lazy.get(name));}}
    private List<String> localNames(){synchronized(GLOBAL){var names=new ArrayList<String>();for(var entry:state.services.entrySet())if(visible(entry.getValue()))names.add(entry.getKey());for(var entry:state.lazy.entrySet())if(visible(entry.getValue()))names.add(entry.getKey());return names;}}
    private static void requireOwner(){
        HostBinderRpc.Peer caller=HostBinderRpc.callingPeer();
        if(caller!=null&&caller.uid()!=android.os.Process.myUid()&&!caller.securityContext().equals("u:r:system_server:s0"))throw new SecurityException("Service registration belongs to its process owner");
    }
    @Override public String updatableViaApex(String name)throws RemoteException{requireFind(name);var parent=parent(name);return !isLocal(name)&&parent!=null?parent.updatableViaApex(name):null;}
    @Override public android.os.ConnectionInfo getConnectionInfo(String name)throws RemoteException{requireFind(name);var parent=parent(name);return !isLocal(name)&&parent!=null?parent.getConnectionInfo(name):null;}
    @Override public void registerClientCallback(String name,IBinder service,IClientCallback callback)throws RemoteException {
        var parent=parent(name);
        if(!isLocal(name)&&parent!=null){parent.registerClientCallback(name,service,callback);return;}
        requireOwner();
        if(remotePolicy()&&!canAddNative(name))throw new SecurityException("Service registration denied");
        Objects.requireNonNull(callback);
        synchronized(GLOBAL){
            Registration registration=state.services.get(name);IBinder registered=visible(registration)?registration.binder():null;
            if(registered==null)throw new IllegalArgumentException("Service does not exist");
            if(registered!=service)throw new IllegalArgumentException("Service does not match its registered endpoint");
            var listeners=state.clientCallbacks.computeIfAbsent(name,key->new HashMap<>());
            if(!listeners.containsKey(callback)){
                IBinder.DeathRecipient death=()->removeClientCallback(name,callback);
                try{callback.asBinder().linkToDeath(death,0);}catch(RemoteException failure){if(listeners.isEmpty())state.clientCallbacks.remove(name);throw failure;}
                listeners.put(callback,death);
            }
        }
        // Original ServiceManager keeps local/RPC services alive when the
        // kernel cannot report node counts (-1). Registration still succeeds;
        // do not fabricate counts or emit a false last-client notification.
    }
    private void removeClientCallback(String name,IClientCallback callback){synchronized(GLOBAL){
        var listeners=state.clientCallbacks.get(name);if(listeners==null)return;
        // Binder has already retired this death recipient before delivery.
        listeners.remove(callback);if(listeners.isEmpty())state.clientCallbacks.remove(name);
    }}
    private void removeClientCallbacks(String name){
        var listeners=state.clientCallbacks.remove(name);if(listeners==null)return;
        for(var entry:listeners.entrySet())try{entry.getKey().asBinder().unlinkToDeath(entry.getValue(),0);}catch(NoSuchElementException alreadyDead){/* Binder death won the retirement race. */}
    }
    public static IServiceManager manager(){return INSTANCE;}
    /** Original ServiceManager publication is shared across the profile. Host
     * bootstrap registrations retain their process owner and are exposed only
     * through that process's explicit peer directory, never as parent defaults. */
    public static IServiceManager publishedManager(){return PUBLISHED;}
    public static IServiceManager client(){return (IServiceManager)HostServiceClients.local(INSTANCE,IServiceManager.DESCRIPTOR);}
    public static IBinder waitForServiceNative(String name){try{return client().getService(name);}catch(RemoteException failure){throw failure.rethrowFromSystemServer();}}
    private static IBinder endpoint(IInterface service,String descriptor){
        Objects.requireNonNull(service);IBinder endpoint=service.asBinder();
        if(endpoint==null){var local=new Binder(descriptor);local.attachInterface(service,descriptor);endpoint=local;}
        return endpoint;
    }
    public static void register(String name,IInterface service,String descriptor){
        INSTANCE.add(name,endpoint(service,descriptor),false);
    }
    private static final class Pending {
        final FutureTask<IBinder> task;final boolean published;final Executor executor;volatile Thread owner;
        Pending(String name,String descriptor,Supplier<? extends IInterface> factory,boolean published,Executor executor){this.published=published;this.executor=Objects.requireNonNull(executor);task=new FutureTask<>(()->{owner=Thread.currentThread();long identity=Binder.clearCallingIdentity();try{IBinder endpoint=endpoint(factory.get(),descriptor);return INSTANCE.add(name,endpoint,published,this)?endpoint:null;}finally{Binder.restoreCallingIdentity(identity);owner=null;}});}
        IBinder get(){if(owner==Thread.currentThread())return null;executor.execute(task);try{return task.get();}catch(InterruptedException failure){Thread.currentThread().interrupt();throw new IllegalStateException("Android service initialization was interrupted",failure);}catch(ExecutionException failure){Throwable cause=failure.getCause();if(cause instanceof RuntimeException error)throw error;if(cause instanceof Error error)throw error;throw new IllegalStateException("Android service initialization failed",cause);}}
    }
    public static void registerLazy(String name,String descriptor,Supplier<? extends IInterface> factory){registerLazy(name,descriptor,factory,false);}
    public static void registerLazy(String name,String descriptor,Supplier<? extends IInterface> factory,boolean published){registerLazy(name,descriptor,factory,published,Runnable::run);}
    /** Original system-server constructors retain their owner Looper even
     * when first requested by a Binder worker. Other lazy owners stay inline. */
    public static void registerLazy(String name,String descriptor,Supplier<? extends IInterface> factory,boolean published,Executor executor){synchronized(GLOBAL){if(GLOBAL.services.containsKey(name)||GLOBAL.lazy.putIfAbsent(name,new Pending(name,descriptor,factory,published,executor))!=null)throw new IllegalStateException("Service already registered: "+name);}}
    public static void unregisterLazy(String name){synchronized(GLOBAL){GLOBAL.lazy.remove(name);}}
    public static void unregister(String name,IInterface service,String descriptor){synchronized(GLOBAL){var registration=GLOBAL.services.get(name);if(registration!=null&&registration.binder().queryLocalInterface(descriptor)==service){GLOBAL.services.remove(name);INSTANCE.removeClientCallbacks(name);INSTANCE.notifyScopes(name,null);}}}
    @Override public IBinder getService(String name)throws RemoteException{
        IBinder result=checkService(name);
        synchronized(GLOBAL){if(result==null&&missing.size()<32&&missing.add(name))
            System.err.println("[android-host] Service could not be found: "+name.replaceAll("[\\p{Cntrl}]","?").substring(0,Math.min(name.length(),128)));
        }
        return result;
    }
    @Override public IBinder checkService(String name)throws RemoteException{
        if(!canFind(name))return null;Pending pending;
        synchronized(GLOBAL){var registration=state.services.get(Objects.requireNonNull(name));if(visible(registration))return registration.binder();pending=state.lazy.get(name);if(!visible(pending))pending=null;}
        if(pending!=null)return pending.get();var parent=parent(name);return parent==null?null:parent.checkService(name);
    }
    @Override public android.os.Service getService2(String name)throws RemoteException{return metadata(getService(name));}
    @Override public android.os.Service checkService2(String name)throws RemoteException{return metadata(checkService(name));}
    private static android.os.Service metadata(IBinder binder){
        var value=new ServiceWithMetadata();value.service=binder;value.isLazyService=false;
        return android.os.Service.serviceWithMetadata(value);
    }
    @Override public void addService(String name,IBinder service,boolean allowIsolated,int flags){add(name,service,true);}
    private void add(String name,IBinder service,boolean published){add(name,service,published,null);}
    private boolean add(String name,IBinder service,boolean published,Pending pending){
        requireOwner();if(remotePolicy()&&!canAddNative(name))throw new SecurityException("Service registration denied");
        Objects.requireNonNull(service);Map<HostServices,List<IServiceCallback>> notifications=new LinkedHashMap<>();
        synchronized(GLOBAL){
            // A factory may finish after its owner has retired or replaced the
            // declaration. Publication belongs to that exact declaration.
            if(pending!=null&&state.lazy.get(name)!=pending){
                // Original SystemService.onStart may publish its own Binder
                // from inside the lazy factory. Return that exact endpoint on
                // the first lookup without reviving a retired declaration.
                Registration registered=state.services.get(name);
                return registered!=null&&registered.binder()==service;
            }
            if(state.retired)throw new IllegalStateException("Android service process has retired");
            if(name==null||name.isBlank()||name.length()>256)throw new IllegalArgumentException("Invalid service name");
            Registration previous=state.services.get(name);
            if(previous!=null&&previous.binder()!=service)throw new IllegalStateException("Service already registered: "+name);
            Registration registered=new Registration(service,published||(previous!=null&&previous.published()));
            state.services.put(name,registered);state.lazy.remove(name);notifyScopes(name,service);
            for(var directory:state.directories)if(directory.visible(registered)){
                directory.missing.remove(name);notifications.put(directory,List.copyOf(directory.callbacks.getOrDefault(name,List.of())));
            }
        }
        for(var entry:notifications.entrySet())for(var callback:entry.getValue())try{callback.onRegistration(name,service);}catch(RemoteException gone){entry.getKey().removeCallback(name,callback);}
        return true;
    }
    @Override public String[] listServices(int flags)throws RemoteException{requireList();var names=new LinkedHashSet<>(localNames());for(var parent:parents())Collections.addAll(names,parent.listServices(flags));return names.toArray(String[]::new);}
    @Override public boolean isDeclared(String name)throws RemoteException{requireFind(name);if(isLocal(name))return true;var parent=parent(name);return parent!=null&&parent.isDeclared(name);}
    @Override public String[] getDeclaredInstances(String descriptor)throws RemoteException{var names=new LinkedHashSet<String>();for(String name:localNames())if(name.startsWith(descriptor+"/"))names.add(name.substring(descriptor.length()+1));for(var parent:parents())Collections.addAll(names,parent.getDeclaredInstances(descriptor));int total=names.size();names.removeIf(instance->!canFind(descriptor+"/"+instance));if(total!=0&&names.isEmpty())throw new SecurityException("Service access denied");return names.toArray(String[]::new);}
    @Override public String[] getUpdatableNames(String apex)throws RemoteException{var names=new LinkedHashSet<String>();for(var parent:parents())Collections.addAll(names,parent.getUpdatableNames(apex));return names.toArray(String[]::new);}
    @Override public void registerForNotifications(String name,IServiceCallback callback)throws RemoteException{
        requireFind(name);Objects.requireNonNull(callback);IBinder service;
        var parent=parent(name);if(!isLocal(name)&&parent!=null){parent.registerForNotifications(name,callback);return;}
        synchronized(GLOBAL){
            var list=callbacks.computeIfAbsent(Objects.requireNonNull(name),key->new ArrayList<>());
            if(!list.contains(callback))list.add(callback);
            var registration=state.services.get(name);service=visible(registration)?registration.binder():null;
        }
        if(service!=null)callback.onRegistration(name,service);
    }
    private boolean removeCallback(String name,IServiceCallback callback){synchronized(GLOBAL){var list=callbacks.get(name);if(list==null)return false;boolean removed=list.remove(callback);if(list.isEmpty())callbacks.remove(name);return removed;}}
    @Override public void unregisterForNotifications(String name,IServiceCallback callback)throws RemoteException{requireFind(name);if(removeCallback(name,callback))return;var parent=parent(name);if(parent!=null)parent.unregisterForNotifications(name,callback);}
    @Override public void tryUnregisterService(String name,IBinder service){requireOwner();synchronized(GLOBAL){var registration=state.services.get(name);if(visible(registration)&&registration.binder()==service){state.services.remove(name);removeClientCallbacks(name);notifyScopes(name,null);}}}
    @Override public ServiceDebugInfo[] getServiceDebugInfo(){requireList();return localNames().stream().map(name->{var info=new ServiceDebugInfo();info.name=name;info.debugPid=state.peer==null?android.os.Process.myPid():state.peer.pid();return info;}).toArray(ServiceDebugInfo[]::new);}
}
