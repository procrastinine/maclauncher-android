package maclauncher.android.framework;

import android.content.pm.ServiceInfo;
import android.os.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.*;

/** One owner for requested processes, authenticated activations and their directories. */
public final class HostProcessServices implements AutoCloseable {
    public record Key(String packageName,String processName){public Key{Objects.requireNonNull(packageName);Objects.requireNonNull(processName);}}
    public record Retirement(HostBinderRpc.Peer peer,String connectionId){}
    public record Directory(HostBinderRpc.Peer peer,Process process,boolean available){}
    private record Activation(Key key,int pid,String connectionId){}
    // Pending requests are adopted by their authenticated connection. A session
    // generation distinguishes reconnects even when both processes remain alive.
    private final Map<Activation,Process> processes=new LinkedHashMap<>();
    private BiConsumer<Key,String> resolver;
    private Consumer<Retirement> retirement;
    private final Set<Consumer<Directory>> directoryObservers=new LinkedHashSet<>();
    private final ExecutorService operations=Executors.newCachedThreadPool(operation->{Thread thread=new Thread(operation,"Android service connection");thread.setDaemon(true);return thread;});
    private boolean closed;
    public void resolver(Consumer<Key> value){Objects.requireNonNull(value);resolver((key,id)->value.accept(key));}
    public synchronized void resolver(BiConsumer<Key,String> value){if(closed)throw new IllegalStateException("Android process directory is closed");resolver=Objects.requireNonNull(value);}
    public synchronized void retirement(Consumer<Retirement> value){if(closed)throw new IllegalStateException("Android process directory is closed");retirement=Objects.requireNonNull(value);}
    /** Observe only ready, authenticated directories; observation never starts a process. */
    public AutoCloseable observeDirectories(Consumer<Directory> observer){
        List<Directory> ready;
        synchronized(this){if(closed)throw new IllegalStateException("Android process directory is closed");directoryObservers.add(Objects.requireNonNull(observer));ready=processes.values().stream().filter(value->!value.retired&&value.root!=null).map(value->new Directory(value.expectedPeer,value,true)).toList();}
        try{for(var value:ready)observer.accept(value);}catch(RuntimeException|Error failure){synchronized(this){directoryObservers.remove(observer);}throw failure;}
        return ()->{synchronized(this){directoryObservers.remove(observer);}};
    }
    private void notifyDirectory(Process process,boolean available){List<Consumer<Directory>> observers;Directory value;synchronized(this){observers=List.copyOf(directoryObservers);value=new Directory(process.expectedPeer,process,available);}for(var observer:observers)observer.accept(value);}
    public final class Process implements AutoCloseable {
        private Activation activation;
        private final String requestId=UUID.randomUUID().toString();
        private HostBinderRpc.Connection connection;
        private HostBinderRpc.Peer expectedPeer;
        private IBinder root;
        private HostServiceDirectory publication;
        private boolean directoryExpected;
        // Lifecycle is reported through the authenticated supervisor, never
        // inferred from possession of a Binder connection or a PID supplied by
        // an application. UNKNOWN remains distinct from a retired process.
        private int processState=android.app.ActivityManager.PROCESS_STATE_UNKNOWN;
        private final CompletableFuture<IServiceManager> directory=new CompletableFuture<>();
        private final IBinder.DeathRecipient death=this::close;
        private boolean retired;
        private Process(Activation activation){this.activation=activation;}
        public HostBinderRpc.Peer identity(){synchronized(HostProcessServices.this){if(retired||connection==null)throw new IllegalStateException("Android process connection is not active");return connection.peer();}}
        private IServiceManager directory()throws RemoteException{
            try{return directory.get();}catch(InterruptedException interrupted){Thread.currentThread().interrupt();throw new RemoteException("Android process connection interrupted");}
            catch(ExecutionException unavailable){throw new DeadObjectException();}
        }
        public IBinder service(String name)throws RemoteException{try{return directory().checkService(Objects.requireNonNull(name));}catch(DeadObjectException gone){close();throw gone;}}
        /** Callback-based Android operations await startup without parking the caller's Looper. */
        public CompletionStage<IBinder> serviceAsync(String name){
            Objects.requireNonNull(name);
            return directory.thenApplyAsync(manager->{
                synchronized(HostProcessServices.this){if(retired)throw new CompletionException(new DeadObjectException());}
                try{return manager.checkService(name);}catch(RemoteException failure){if(failure instanceof DeadObjectException)close();throw new CompletionException(failure);}
            },operations).minimalCompletionStage();
        }
        public String[] services()throws RemoteException{try{return directory().listServices(IServiceManager.DUMP_FLAG_PRIORITY_ALL);}catch(DeadObjectException gone){close();throw gone;}}
        private void observe(HostBinderRpc.Connection owned){owned.closed().whenComplete((unused,failure)->close());}
        private Runnable retire(){
            HostBinderRpc.Connection owned;IBinder binder;HostServiceDirectory published;Consumer<Retirement> notify;Retirement ended;HostBinderRpc.Peer retiredPeer;
            synchronized(HostProcessServices.this){
                if(retired)return ()->{};
                int before=expectedPeer==null?android.app.ActivityManager.PROCESS_STATE_NONEXISTENT:uidProcessState(expectedPeer.uid());
                retired=true;processes.remove(activation,this);owned=connection;binder=root;published=publication;publication=null;
                retiredPeer=expectedPeer!=null&&processes.values().stream().noneMatch(value->expectedPeer.equals(value.expectedPeer))?expectedPeer:null;
                if(expectedPeer!=null)notifyUidChange(expectedPeer.uid(),before);
                notify=retirement;
                ended=activation.connectionId==null||expectedPeer==null?null:new Retirement(expectedPeer,activation.connectionId);
            }
            return ()->{
                directory.completeExceptionally(new DeadObjectException());
                if(binder!=null)notifyDirectory(this,false);
                if(published!=null)published.close();
                if(retiredPeer!=null)HostServices.retirePeer(retiredPeer);
                if(binder!=null)try{binder.unlinkToDeath(death,0);}catch(NoSuchElementException ignored){}
                if(owned!=null)owned.close();
                if(ended!=null&&notify!=null)notify.accept(ended);
            };
        }
        @Override public void close(){retire().run();}
    }
    private static Activation activation(HostBinderRpc.Peer peer,String connectionId){return new Activation(new Key(peer.packageName(),peer.processName()),peer.pid(),connectionId);}
    private Process enroll(HostBinderRpc.Peer peer,String connectionId,boolean directoryExpected){
        if(closed)throw new IllegalStateException("Android process directory is closed");
        Activation id=activation(peer,connectionId);Process process=processes.get(id);
        if(process==null){
            process=directoryExpected?processes.remove(new Activation(id.key,0,null)):null;
            if(process==null)process=new Process(id);else process.activation=id;
            processes.put(id,process);
        }
        if(process.expectedPeer!=null&&!process.expectedPeer.equals(peer))throw new SecurityException("Android process identity differs from its authenticated activation");
        process.expectedPeer=peer;process.directoryExpected|=directoryExpected;return process;
    }
    /** Bind the authenticated activation before asynchronous socket setup. */
    public void expect(HostBinderRpc.Peer peer){expect(peer,null);}
    public synchronized void expect(HostBinderRpc.Peer peer,String connectionId){enroll(peer,connectionId,true);}
    /** Authorizing an incoming caller does not publish that caller's process directory. */
    public synchronized void expectIncoming(HostBinderRpc.Peer peer){enroll(peer,null,false);}
    /** Kernel-authenticated listener connections consult this same owner. */
    public synchronized HostBinderRpc.Peer expectedIdentity(int pid){
        if(closed)return null;
        for(Process process:processes.values())if(!process.retired&&process.expectedPeer!=null&&process.expectedPeer.pid()==pid)return process.expectedPeer;
        return null;
    }
    /** A directory lookup cannot revive a retired authenticated connection. */
    synchronized IServiceManager serviceDirectory(HostBinderRpc.Peer peer)throws DeadObjectException{
        if(!peer.equals(expectedIdentity(peer.pid())))throw new DeadObjectException();
        return HostServices.scopedManager(peer);
    }
    public synchronized void processState(HostBinderRpc.Peer peer,int state){
        if(state<android.app.ActivityManager.PROCESS_STATE_PERSISTENT||state>=android.app.ActivityManager.PROCESS_STATE_NONEXISTENT)
            throw new IllegalArgumentException("Invalid Android process state");
        Process process=processes.get(activation(peer,null));
        if(closed||process==null||process.retired||!peer.equals(process.expectedPeer))
            throw new SecurityException("Android process lifecycle has no active enrollment");
        int before=uidProcessState(peer.uid());process.processState=state;notifyUidChange(peer.uid(),before);
    }
    private void notifyUidChange(int uid,int before){int after=uidProcessState(uid);if(after!=before)HostActivityManager.hostDirectoryUidChanged(this,uid,after);}
    /** Snapshot the canonical activations; queries cannot enroll or revive a process. */
    public synchronized void processStates(int[] pids,int[] states){
        Objects.requireNonNull(pids,"pids");Objects.requireNonNull(states,"states");
        if(pids.length!=states.length)throw new IllegalArgumentException("pids and states arrays have different lengths");
        Arrays.fill(states,android.app.ActivityManager.PROCESS_STATE_NONEXISTENT);
        for(int i=0;i<pids.length;i++)for(Process process:processes.values())
            if(!process.retired&&process.expectedPeer!=null&&process.expectedPeer.pid()==pids[i]){
                if(states[i]==android.app.ActivityManager.PROCESS_STATE_NONEXISTENT||process.processState>=0)states[i]=process.processState;
            }
    }
    /** Aggregate only live authenticated activations belonging to this UID. */
    public synchronized int uidProcessState(int uid){
        int state=android.app.ActivityManager.PROCESS_STATE_NONEXISTENT;boolean live=false;
        for(Process process:processes.values())if(!process.retired&&process.expectedPeer!=null&&process.expectedPeer.uid()==uid){live=true;if(process.processState>=0)state=Math.min(state,process.processState);}
        return live&&state==android.app.ActivityManager.PROCESS_STATE_NONEXISTENT?android.app.ActivityManager.PROCESS_STATE_UNKNOWN:state;
    }
    /** The native listener transfers its original session into this same owner. */
    public void attachIncoming(long pointer,HostBinderRpc.Peer peer){
        var connection=HostBinderRpc.accepted(pointer,peer);Process process;
        try{
            synchronized(this){
                process=processes.get(activation(peer,null));
                if(closed||process==null||process.retired||!peer.equals(process.expectedPeer))
                    throw new SecurityException("Android process enrollment ended before connection");
                if(process.connection!=null)throw new IllegalStateException("Android process connection is already active");
                process.connection=connection;
            }
            process.observe(connection);
        }catch(Throwable failure){connection.close();throw failure;}
    }
    /** A received connection immediately joins this owner, before root exchange. */
    public Process stage(HostBinderRpc.Connection connection){return stage(connection,null);}
    public Process stage(HostBinderRpc.Connection connection,String connectionId){
        Objects.requireNonNull(connection);Process process;
        synchronized(this){
            try{
                process=connectionId==null?enroll(connection.peer(),null,true):processes.get(activation(connection.peer(),connectionId));
                if(closed||process==null||process.retired||!connection.peer().equals(process.expectedPeer))throw new IllegalStateException("Android process enrollment ended before connection");
            }catch(RuntimeException failure){connection.close();throw failure;}
            if(process.connection==connection)return process;
            if(process.connection!=null){connection.close();throw new IllegalStateException("Android process connection is already active");}
            process.connection=connection;
        }
        process.observe(connection);return process;
    }
    public Process attach(HostBinderRpc.Connection connection,IBinder root)throws RemoteException{return attach(connection,root,null);}
    public Process attach(HostBinderRpc.Connection connection,IBinder root,String connectionId)throws RemoteException{
        Process process=stage(connection,connectionId);
        try{
            // Actual remote calls never hold the process directory lock.
            if(!IServiceManager.DESCRIPTOR.equals(root.getInterfaceDescriptor()))throw new IllegalArgumentException("Android process did not publish a service directory");
            root.linkToDeath(process.death,0);
            synchronized(this){if(process.retired){root.unlinkToDeath(process.death,0);throw new DeadObjectException();}if(process.root!=null){root.unlinkToDeath(process.death,0);if(process.root!=root)throw new IllegalStateException("Android process directory was already published");return process;}process.root=root;}
            process.directory.complete(IServiceManager.Stub.asInterface(root));
            notifyDirectory(process,true);
            return process;
        }catch(RemoteException|RuntimeException failure){process.close();throw failure;}
    }
    /** Complete the reverse directory on the already authenticated listener session. */
    void attachDirectory(HostBinderRpc.Peer peer,IBinder root)throws RemoteException{
        HostBinderRpc.Connection connection;synchronized(this){Process process=processes.get(activation(peer,null));if(closed||process==null||process.retired||!peer.equals(process.expectedPeer)||process.connection==null)throw new DeadObjectException();connection=process.connection;}
        attach(connection,Objects.requireNonNull(root));
    }
    /** One publication lease belongs to the same connection as its remote directory. */
    void publishDirectory(HostBinderRpc.Peer peer,android.content.Context context)throws RemoteException{
        Process process;IBinder remote;HostServiceDirectory local;
        synchronized(this){process=find(peer);if(closed||process==null||process.retired||process.root==null)throw new DeadObjectException();if(process.publication!=null)return;remote=process.root;local=new HostServiceDirectory(context);process.publication=local;}
        try{HostServiceDirectory.publish(remote,local.root());}catch(RemoteException|RuntimeException failure){synchronized(this){if(process.publication==local)process.publication=null;}local.close();throw failure;}
    }
    public Process find(ServiceInfo service){Objects.requireNonNull(service);return find(service.packageName,service.processName==null?service.packageName:service.processName);}
    public synchronized Process find(HostBinderRpc.Peer peer){Process result=null;for(Process value:processes.values())if(value.directoryExpected&&peer.equals(value.expectedPeer))result=value;return result;}
    public Process find(String packageName,String processName){
        Key key=new Key(packageName,processName);Process process=null;BiConsumer<Key,String> request;
        synchronized(this){
            if(closed)return null;
            for(var entry:processes.entrySet())if(entry.getKey().key.equals(key)&&entry.getValue().directoryExpected)process=entry.getValue();
            if(process!=null)return process;request=resolver;if(request==null)return null;
            Activation id=new Activation(key,0,null);process=new Process(id);process.directoryExpected=true;processes.put(id,process);
        }
        try{request.accept(key,process.requestId);}catch(RuntimeException failure){process.close();throw failure;}return process;
    }
    public void unavailable(String packageName,String processName){
        List<Process> snapshot;synchronized(this){Key key=new Key(packageName,processName);snapshot=processes.entrySet().stream().filter(entry->entry.getKey().key.equals(key)).map(Map.Entry::getValue).toList();}
        for(Process process:snapshot)process.close();
    }
    /** A failed demand cannot retire a later request or an adopted connection. */
    public void unavailable(Key key,String requestId){Runnable cleanup;synchronized(this){Process process=processes.get(new Activation(key,0,null));if(process==null||!process.requestId.equals(requestId))return;cleanup=process.retire();}cleanup.run();}
    public void unavailable(HostBinderRpc.Peer peer,String connectionId){Process process;synchronized(this){process=processes.get(activation(peer,connectionId));if(process==null||!peer.equals(process.expectedPeer))return;}process.close();}
    /** Process death retires every session for that exact authenticated PID. */
    public void unavailable(HostBinderRpc.Peer peer){List<Process> snapshot;synchronized(this){snapshot=processes.values().stream().filter(process->peer.equals(process.expectedPeer)).toList();}for(Process process:snapshot)process.close();}
    @Override public void close(){List<Process> snapshot;synchronized(this){if(closed)return;closed=true;snapshot=List.copyOf(processes.values());processes.clear();resolver=null;retirement=null;}try{for(Process process:snapshot)process.close();}finally{operations.shutdown();}}
}
