package maclauncher.android;

import android.content.Context;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import maclauncher.android.framework.*;
import org.json.*;

/** The existing supervisor channel owns process demand and received socket capabilities. */
public final class ProcessAuthority implements AutoCloseable {
    record Identity(String packageName,String processName,int pid,int hostPid,int uid,int hostUid,String securityContext){HostBinderRpc.Peer peer(){return new HostBinderRpc.Peer(packageName,processName,pid,uid,securityContext);}}
    private record Packet(byte[] bytes,int fd){}
    final Identity self;
    private final JSONObject platform,accounts,serviceOwners,composition;
    private volatile JSONObject system;
    private final ParcelFileDescriptor descriptor;
    private final OutputStream output;
    private final Set<ParcelFileDescriptor> pendingSockets=new HashSet<>();
    private final CompletableFuture<Context> applicationContext=new CompletableFuture<>();
    private final CompletableFuture<Void> disconnected=new CompletableFuture<>();
    private boolean applicationReader;
    private static volatile ProcessAuthority active;
    private final java.util.concurrent.atomic.AtomicLong clipboardSequence=new java.util.concurrent.atomic.AtomicLong();
    private final Map<String,CompletableFuture<String>> clipboardRequests=new ConcurrentHashMap<>();
    private final java.util.concurrent.atomic.AtomicInteger pendingProcessState=new java.util.concurrent.atomic.AtomicInteger(-1);
    private final java.util.concurrent.atomic.AtomicBoolean sendingProcessState=new java.util.concurrent.atomic.AtomicBoolean();
    private final ExecutorService connections=Executors.newCachedThreadPool(task->{Thread thread=new Thread(task,"Android process connection");thread.setDaemon(true);return thread;});
    private volatile boolean closed;
    private ProcessAuthority(Identity self,ParcelFileDescriptor descriptor,JSONObject platform,JSONObject accounts,JSONObject system,JSONObject serviceOwners,JSONObject composition){this.self=self;this.descriptor=descriptor;this.platform=platform;this.accounts=accounts;this.system=system;this.serviceOwners=serviceOwners;this.composition=composition;output=new FileOutputStream(descriptor.getFileDescriptor());}
    private static Identity identity(JSONObject value)throws JSONException{
        Identity result=new Identity(value.getString("packageName"),value.getString("processName"),value.getInt("pid"),value.getInt("hostPid"),value.getInt("uid"),value.getInt("hostUid"),value.optString("securityContext","u:r:untrusted_app:s0"));
        if(result.packageName.isBlank()||result.processName.isBlank()||result.pid<2||result.pid>65535||result.hostPid<2||result.uid<0||result.hostUid<0)throw new IllegalArgumentException("Invalid Android process authority");return result;
    }
    static ProcessAuthority read(LaunchConfig config)throws Exception{
        if(!config.contains("authorityFd"))return null;
        return read(config.descriptor("authorityFd"),config.text("packageName"),config.serviceProcess()?config.text("processName"):null);
    }
    static ProcessAuthority read(int fd,String expectedPackage,String expectedProcess)throws Exception{
        ParcelFileDescriptor descriptor=ParcelFileDescriptor.adoptFd(fd);
        try{
            var file=descriptor.getFileDescriptor();int flags=Os.fcntlInt(file,OsConstants.F_GETFL,0);Os.fcntlInt(file,OsConstants.F_SETFL,flags&~OsConstants.O_NONBLOCK);
            JSONObject helloValue=new JSONObject().put("op","identity").put("pid",Os.getpid()).put("hostPid",HostProcessIdentity.physicalPid());
            String androidId=HostAndroidIdentity.localId();if(androidId!=null)helloValue.put("androidId",androidId);
            byte[] hello=helloValue.toString().getBytes(StandardCharsets.UTF_8);
            DataOutputStream identityOutput=new DataOutputStream(new FileOutputStream(file));identityOutput.writeInt(hello.length);identityOutput.write(hello);identityOutput.flush();
            Packet packet=receiveNative(descriptor.getFd());if(packet==null)throw new EOFException("Android process authority ended before initialization");
            if(packet.fd>=0){ParcelFileDescriptor.adoptFd(packet.fd).close();throw new IOException("Unexpected Android bootstrap descriptor");}
            JSONObject root=new JSONObject(new String(packet.bytes,StandardCharsets.UTF_8));if(root.getInt("schemaVersion")!=2)throw new IOException("Android process authority version is unsupported");Identity self=identity(root.getJSONObject("self"));
            if(self.hostPid!=HostProcessIdentity.physicalPid()||self.pid!=Os.getpid()||!self.packageName.equals(expectedPackage)||expectedProcess!=null&&!self.processName.equals(expectedProcess))throw new SecurityException("Android process authority differs from its actual process");
            // Initial profile metadata precedes application/provider creation;
            // acquiring a lazy system service is not needed to establish it.
            if(root.has("bootCount"))HostContext.hostBootCount(root.getInt("bootCount"));
            else if(self.uid==1000&&self.packageName.equals("android")&&self.processName.equals("android.system")&&root.getJSONObject("self").has("bootCount"))
                HostContext.hostBootCount(root.getJSONObject("self").getInt("bootCount"));
            else if(root.optJSONObject("system")!=null){
                JSONObject owner=root.getJSONObject("system").getJSONObject("identity");profileIdentity("system",owner);
                if(owner.has("bootCount"))HostContext.hostBootCount(owner.getInt("bootCount"));
            }
            JSONObject services=root.optJSONObject("serviceOwners");
            JSONObject policy=root.optJSONObject("accountPolicy");
            if(policy!=null&&!policy.optBoolean("interactive",false)){HostAccountAccess.anonymous();HostGoogleProviders.accountInteraction(false);}
            return new ProcessAuthority(self,descriptor,root.optJSONObject("platform"),root.optJSONObject("accounts"),root.optJSONObject("system"),services,root.optJSONObject("composition"));
        }catch(Throwable failure){descriptor.close();throw failure;}
    }
    maclauncher.android.bridge.NativeComposition.Session composition()throws IOException,JSONException{
        if(composition==null)return null;
        Identity peer=identity(composition.getJSONObject("identity"));
        if(peer.uid()!=1000||!peer.packageName().equals("android")||!peer.processName().equals("android.composition")||!peer.securityContext().equals("u:r:system_server:s0"))throw new SecurityException("Android composition owner has an invalid enrolled identity");
        return new maclauncher.android.bridge.NativeComposition.Session(composition.getString("socketPath"),peer.peer(),composition.getJSONObject("display"));
    }
    void attachPlatform()throws IOException,JSONException,android.os.RemoteException{
        declareServices();
        startApplicationReader();
        if(accounts!=null){var peer=profileIdentity("accounts",accounts.getJSONObject("identity"));HostRemoteServices.attachAccounts(accounts.getString("socketPath"),peer.peer(),null);}
        if(system!=null){var owner=system.getJSONObject("identity");var peer=profileIdentity("system",owner);HostRemoteServices.attachPlatform(system.getString("socketPath"),peer.peer());}
        if(platform==null)return;
        var peer=identity(platform.getJSONObject("identity"));
        if(peer.uid!=1017||!"u:r:keystore:s0".equals(peer.securityContext))throw new SecurityException("Android platform owner has an invalid role");
        HostRemoteServices.attachPlatform(platform.getString("socketPath"),peer.peer());
    }
    private void declareServices()throws JSONException{
        JSONObject account=serviceOwners==null?null:serviceOwners.optJSONObject(Context.ACCOUNT_SERVICE);
        if(account==null&&accounts!=null)account=accounts.getJSONObject("identity");
        if(account!=null&&!isSelf(account))HostRemoteServices.accountsOwner(account.getString("packageName"),account.getString("processName"));
        JSONObject keyStore=serviceOwners==null?null:serviceOwners.optJSONObject("keystore");
        if(keyStore!=null&&!isSelf(keyStore))HostRemoteServices.declare(processKey(keyStore),HostKeyStore.declarations());
        JSONObject shared=serviceOwners==null?null:serviceOwners.optJSONObject("system");
        if(shared!=null&&!isSelf(shared)){
            var key=processKey(shared);HostRemoteServices.platformOwner(key);HostRemoteServices.declare(key,HostSystemServices.declarations());
        }
    }
    private boolean isSelf(JSONObject value)throws JSONException{return self.packageName.equals(value.getString("packageName"))&&self.processName.equals(value.getString("processName"));}
    private static HostProcessServices.Key processKey(JSONObject value)throws JSONException{return new HostProcessServices.Key(value.getString("packageName"),value.getString("processName"));}
    private static Identity profileIdentity(String role,JSONObject value)throws JSONException{
        var peer=identity(value);boolean valid=switch(role){
            case "accounts"->peer.uid==1000&&peer.packageName.equals("android")&&peer.securityContext.equals("u:r:system_server:s0");
            case "system"->peer.uid==1000&&peer.packageName.equals("android")&&peer.processName.equals("android.system")&&peer.securityContext.equals("u:r:system_server:s0");
            case "keystore"->peer.uid==1017&&peer.packageName.equals("android.keystore")&&peer.securityContext.equals("u:r:keystore:s0");
            default->false;
        };if(!valid)throw new SecurityException("Android profile service has an invalid role");return peer;
    }
    private synchronized void send(JSONObject message)throws IOException{
        if(closed)throw new EOFException("Android supervisor channel is closed");byte[] bytes=message.toString().getBytes(StandardCharsets.UTF_8);DataOutputStream stream=new DataOutputStream(output);stream.writeInt(bytes.length);stream.write(bytes);stream.flush();
    }
    private void request(HostProcessServices.Key process,String requestId){
        try{JSONObject message=new JSONObject();message.put("op","connect");message.put("packageName",process.packageName());message.put("processName",process.processName());message.put("requestId",requestId);send(message);}
        catch(IOException|JSONException failure){throw new IllegalStateException("Android process request failed",failure);}
    }
    private void retired(HostProcessServices.Retirement retirement){
        if(closed)return;
        var peer=retirement.peer();
        try{send(new JSONObject().put("op","disconnect").put("connectionId",retirement.connectionId()).put("peer",new JSONObject()
            .put("packageName",peer.packageName()).put("processName",peer.processName()).put("pid",peer.pid()).put("uid",peer.uid()).put("hostUid",self.hostUid).put("securityContext",peer.securityContext())));}
        catch(IOException|JSONException failure){if(!closed)System.err.println("[android-host] Process retirement notification unavailable: "+failure.getMessage());}
    }
    void windowCloseRequested(){
        try{send(new JSONObject().put("op","window-close"));}
        catch(IOException|JSONException failure){System.err.println("[android-host] Window close notification unavailable: "+failure.getMessage());}
    }
    void operationResult(String id,String status,int code)throws IOException,JSONException{
        if(!Set.of("licensed","not-licensed","unavailable","failure","timeout").contains(status))throw new IllegalArgumentException("Invalid Android operation result");
        send(new JSONObject().put("op","operation-result").put("id",id).put("status",status).put("code",code));
    }
    /** Clipboard transfer is authorized by the launcher's physical-key/foreground
     * check. Android clipboard APIs retain their separate application storage. */
    public static CompletableFuture<String> clipboard(String operation,String text){
        ProcessAuthority owner=active;
        if(owner==null||owner.closed||!("copy".equals(operation)||"paste".equals(operation))||"copy".equals(operation)&&text==null)
            return CompletableFuture.completedFuture(null);
        String id=Long.toUnsignedString(owner.clipboardSequence.incrementAndGet());
        CompletableFuture<String> result=new CompletableFuture<>();owner.clipboardRequests.put(id,result);
        result.completeOnTimeout(null,2,TimeUnit.SECONDS).whenComplete((value,failure)->owner.clipboardRequests.remove(id,result));
        try{owner.connections.execute(()->{
            try{
                JSONObject message=new JSONObject().put("op","clipboard").put("requestId",id).put("operation",operation);
                if(operation.equals("copy"))message.put("text",text);
                if(message.toString().getBytes(StandardCharsets.UTF_8).length>1024*1024){result.complete(null);return;}
                if(!result.isDone())owner.send(message);
            }catch(IOException|JSONException failure){result.complete(null);}
        });}catch(RejectedExecutionException stopped){result.complete(null);}
        return result;
    }
    private interface Receiver {boolean receive(JSONObject message,ParcelFileDescriptor socket)throws IOException,JSONException;}
    /** A lifecycle transition carries no caller-supplied PID or UID. The parent
     * binds this inherited channel to its enrolled child. Coalescing bounds the
     * queue without doing socket I/O while an Activity lifecycle lock is held. */
    public static void processStateChanged(int state){
        var owner=active;
        if(owner==null||owner.closed||owner.system==null||owner.system.optJSONObject("identity").optInt("processStateVersion",0)!=1)return;
        owner.pendingProcessState.set(state);owner.flushProcessState();
    }
    private void flushProcessState(){
        if(closed||!sendingProcessState.compareAndSet(false,true))return;
        try{connections.execute(()->{
            try{int state;while(!closed&&(state=pendingProcessState.getAndSet(-1))>=0)send(new JSONObject().put("op","process-state").put("state",state));}
            catch(IOException|JSONException failure){if(!closed)System.err.println("[android-host] Process lifecycle channel unavailable: "+failure.getClass().getSimpleName());}
            finally{sendingProcessState.set(false);if(!closed&&pendingProcessState.get()>=0)flushProcessState();}
        });}catch(RejectedExecutionException stopped){sendingProcessState.set(false);}
    }
    private void startApplicationReader(){
        if(applicationReader)return;applicationReader=true;
        active=this;
        HostRemoteServices.resolver(this::request);
        HostRemoteServices.retirement(this::retired);
        monitor((message,socket)->{
            String operation=message.getString("op");
            if(operation.equals("unavailable")){
                if(socket!=null)throw new IOException("Unexpected unavailable-process descriptor");
                if(message.has("peer")){
                    var peer=identity(message.getJSONObject("peer")).peer();
                    if(message.has("connectionId"))HostRemoteServices.unavailable(peer,message.getString("connectionId"));else HostRemoteServices.unavailable(peer);
                }else HostRemoteServices.unavailable(new HostProcessServices.Key(message.getString("packageName"),message.getString("processName")),message.getString("requestId"));
                return false;
            }
            if(!operation.equals("connect"))throw new IOException("Invalid Android process connection message");
            if(socket==null){
                String role=message.getString("role"),connectionId=message.getString("connectionId");
                Identity peer=profileIdentity(role,message.getJSONObject("peer"));
                JSONObject declared=serviceOwners==null?null:serviceOwners.optJSONObject(role.equals("accounts")?Context.ACCOUNT_SERVICE:role);
                if(declared==null||!peer.packageName.equals(declared.getString("packageName"))||!peer.processName.equals(declared.getString("processName")))throw new SecurityException("Android profile service was not declared by its supervisor");
                HostRemoteServices.expect(peer.peer(),connectionId);
                try{connections.execute(()->{
                    try{
                        if(role.equals("accounts"))HostRemoteServices.attachAccounts(message.getString("socketPath"),peer.peer(),connectionId);
                        else HostRemoteServices.attachPlatform(message.getString("socketPath"),peer.peer(),connectionId);
                        if(role.equals("system")){system=new JSONObject().put("identity",message.getJSONObject("peer"));processStateChanged(HostActivityManager.hostUidProcessState(null,android.os.Process.myUid()));}
                    }catch(Throwable failure){HostRemoteServices.unavailable(peer.peer(),connectionId);if(!closed)System.err.println("[android-host] Android profile service connection unavailable: "+role+" ("+failure+")");}
                });}catch(RejectedExecutionException ended){if(!closed)throw ended;}return false;
            }
            Identity peer=identity(message.getJSONObject("peer"));boolean server=message.getBoolean("server");String connectionId=message.getString("connectionId");HostRemoteServices.expect(peer.peer(),connectionId);
            var owned=socket;synchronized(pendingSockets){if(closed)return false;pendingSockets.add(owned);}
            try{connections.execute(()->{try(owned){HostServiceDirectory.connect(owned,peer.peer(),applicationContext.get(),4,server,connectionId);}
                catch(Throwable failure){HostRemoteServices.unavailable(peer.peer(),connectionId);if(!closed)System.err.println("[android-host] Android process connection unavailable: "+peer.processName+" ("+failure+")");}
                finally{synchronized(pendingSockets){pendingSockets.remove(owned);}}});return true;}
            catch(RejectedExecutionException ended){synchronized(pendingSockets){pendingSockets.remove(owned);}if(!closed)throw ended;return false;}
        },()->{
            // A synchronous lazy factory may be parked on the main Looper.
            // Release its existing directory future before posting any UI stop.
            try{failApplicationRequests();}finally{disconnected.complete(null);}
        });
    }
    void monitor(Context context,Runnable stopped){
        startApplicationReader();applicationContext.complete(Objects.requireNonNull(context));
        disconnected.thenRun(stopped);
        processStateChanged(HostActivityManager.hostUidProcessState(null,android.os.Process.myUid()));
    }
    private void failApplicationRequests(){try{HostRemoteServices.authorityClosed();}finally{applicationContext.completeExceptionally(new EOFException("Android supervisor channel closed"));}}
    void monitorPlatform(HostProcessServices processes,Runnable disconnected)throws IOException,JSONException{monitorPlatform(processes,disconnected,false);}
    void monitorPlatform(HostProcessServices processes,Runnable disconnected,boolean processState)throws IOException,JSONException{
        monitor((message,socket)->{
            if(socket!=null)throw new IOException("Unexpected Android platform enrollment descriptor");
            String operation=message.getString("op");var peer=identity(message.getJSONObject("peer")).peer();
            String requestId=message.getString("requestId"),reply;Set<String> preparedRoots=null;
            try{
                if(operation.equals("enroll")){
                    JSONObject authorization=message.optJSONObject("authorization");
                    if(processState&&authorization!=null){
                        JSONArray packages=authorization.getJSONArray("packages");boolean matches=false;
                        for(int i=0;i<packages.length();i++){JSONObject value=packages.getJSONObject(i);if(peer.packageName().equals(value.getString("packageName"))&&peer.uid()==value.getInt("uid"))matches=true;}
                        if(!matches)throw new SecurityException("Installed package differs from its enrolled process");
                        // The same catalog feeds native package queries and the
                        // original audio permission provider's package observer.
                        preparedRoots=HostPackages.enrollInstalled(peer,packages.toString());
                    }
                    processes.expect(peer);reply="enrolled";
                }
                else if(operation.equals("retire")){processes.unavailable(peer);reply="retired";}
                else if(operation.equals("process-state")&&processState){processes.processState(peer,message.getInt("state"));reply="state-updated";}
                else throw new IOException("Invalid Android platform authority message");
            }catch(Exception rejected){
                // Enrollment can end while a lifecycle notification is in flight.
                // A rejected request belongs to its caller, not to every client
                // of the profile service. Protocol/transport failures still close
                // the authority channel through the common monitor below.
                send(new JSONObject().put("op","failed").put("requestId",requestId)
                    .put("failureType",rejected.getClass().getName()).put("error",rejected.getMessage()));
                return false;
            }
            JSONObject response=new JSONObject().put("op",reply).put("requestId",requestId).put("pid",peer.pid());
            if(preparedRoots!=null)response.put("preparedRoots",new JSONArray(preparedRoots));send(response);
            return false;
        },disconnected);
        JSONObject ready=new JSONObject().put("op","ready");if(processState)ready.put("processStateVersion",1).put("packageSourceLifetimeVersion",1);send(ready);
    }
    private void monitor(Receiver receiver,Runnable disconnected){
        Thread thread=new Thread(()->{
            try{
                while(!closed){Packet packet=receiveNative(descriptor.getFd());if(packet==null)break;ParcelFileDescriptor socket=packet.fd<0?null:ParcelFileDescriptor.adoptFd(packet.fd);
                    try{
                        JSONObject message=new JSONObject(new String(packet.bytes,StandardCharsets.UTF_8));
                        if("clipboard".equals(message.optString("op"))){
                            if(socket!=null)throw new IOException("Unexpected Android clipboard descriptor");
                            CompletableFuture<String> result=clipboardRequests.get(message.getString("requestId"));
                            if(result!=null)result.complete(message.optBoolean("allowed",false)?message.getString("text"):null);
                        }else if(receiver.receive(message,socket))socket=null;
                    }finally{if(socket!=null)socket.close();}
                }
            }catch(IOException|JSONException failure){if(!closed)System.err.println("[android-host] Android supervisor channel closed: "+failure.getMessage());}
            finally{for(var request:clipboardRequests.values())request.complete(null);clipboardRequests.clear();if(!closed)disconnected.run();}
        },"Android process supervisor");thread.setDaemon(true);thread.start();
    }
    @Override public void close()throws IOException{closed=true;if(active==this)active=null;if(applicationReader)failApplicationRequests();for(var request:clipboardRequests.values())request.complete(null);clipboardRequests.clear();connections.shutdown();shutdownNative(descriptor.getFd());descriptor.close();synchronized(pendingSockets){for(var socket:pendingSockets)shutdownNative(socket.getFd());}}
    private static native Packet receiveNative(int descriptor)throws IOException;
    private static native void shutdownNative(int descriptor);
}
