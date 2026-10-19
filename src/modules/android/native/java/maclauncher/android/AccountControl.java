package maclauncher.android;

import android.accounts.*;
import android.content.*;
import android.os.*;
import android.system.Os;
import android.system.OsConstants;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.*;
import maclauncher.android.framework.*;

/** Commands from the launcher to the actual installed account owner. */
final class AccountControl implements AutoCloseable {
    /** Keeps the real Intent, its response Binder and Activity result inside Android. */
    interface ActivityLauncher { CompletionStage<Void> launch(Intent intent); }
    private final Context context;
    private final Handler handler;
    private final AccountManager accounts;
    private final ActivityLauncher activities;
    private final ParcelFileDescriptor socket;
    private final DataInputStream input;
    private final DataOutputStream output;
    private final Map<String,Operation> pending=new ConcurrentHashMap<>();
    private final AutoCloseable listener;
    private final AtomicBoolean closed=new AtomicBoolean();
    private final Runnable disconnected;
    private final Thread reader;
    private final ExecutorService writer=Executors.newSingleThreadExecutor(task->{Thread thread=new Thread(task,"Android account replies");thread.setDaemon(true);return thread;});

    AccountControl(Context context,ParcelFileDescriptor connection,String socketPath,ActivityLauncher activities,Runnable disconnected)throws IOException {
        this.context=Objects.requireNonNull(context);this.activities=activities;this.disconnected=Objects.requireNonNull(disconnected);
        handler=new Handler(context.getMainLooper());accounts=context.getSystemService(AccountManager.class);
        if(accounts==null)throw new IOException("Android account service is unavailable");
        socket=connection.dup();
        DataInputStream source=null;DataOutputStream sink=null;
        try{
            var fd=socket.getFileDescriptor();Os.fcntlInt(fd,OsConstants.F_SETFL,Os.fcntlInt(fd,OsConstants.F_GETFL,0)&~OsConstants.O_NONBLOCK);
            source=new DataInputStream(new ParcelFileDescriptor.AutoCloseInputStream(socket.dup()));
            sink=new DataOutputStream(new ParcelFileDescriptor.AutoCloseOutputStream(socket.dup()));
        }catch(Exception failure){for(var owned:new AutoCloseable[]{source,sink,socket})if(owned!=null)try{owned.close();}catch(Exception cleanup){failure.addSuppressed(cleanup);}writer.shutdownNow();throw new IOException("Android account control connection could not open",failure);}
        input=source;output=sink;
        try{listener=socketPath==null?null:HostRemoteServices.listen(socketPath,context);}catch(IOException failure){source.close();sink.close();socket.close();writer.shutdownNow();throw failure;}
        reader=new Thread(this::read,"Android account control");reader.setDaemon(true);reader.start();
    }
    private void read(){
        try{
            while(!closed.get()){
                int length=input.readInt();if(length<=0)throw new IOException("Invalid Android account command length");
                byte[] bytes=input.readNBytes(length);if(bytes.length!=length)throw new EOFException();
                JSONObject request=new JSONObject(new String(bytes,StandardCharsets.UTF_8));
                if(!handler.post(()->dispatch(request)))break;
            }
        }catch(EOFException disconnected){}catch(Exception failure){if(!closed.get())System.err.println("[android-host] Account control stopped: "+failure.getClass().getSimpleName());}
        finally{close(true);}
    }
    private void dispatch(JSONObject request){
        if(closed.get())return;
        String id=request.optString("id","");
        if(request.optInt("schemaVersion",0)!=1||!id.matches("[1-9][0-9]*")){close(true);return;}
        try{
            switch(request.getString("command")){
                case "initializeServices"->{
                    HostPackages.inspectInstalled(request.getJSONObject("payload").getJSONArray("packages").toString());
                    HostUriGrants.start(context);HostShortcuts.start(context);reply(id,true);
                }
                case "removePackages"->{
                    JSONArray packages=request.getJSONObject("payload").getJSONArray("packages");
                    for(int i=0;i<packages.length();i++)HostUriGrants.removePackage(packages.getString(i));
                    HostUriGrants.flush();reply(id,true);
                }
                case "accounts"->reply(id,inventory());
                case "cancel"->{Operation operation=pending.get(request.getJSONObject("payload").getString("id"));if(operation!=null)operation.cancel();}
                case "enroll","retire"->{
                    JSONObject payload=request.getJSONObject("payload"),value=payload.getJSONObject("peer");
                    var peer=new HostBinderRpc.Peer(value.getString("packageName"),value.getString("processName"),value.getInt("pid"),value.getInt("uid"),value.optString("securityContext","u:r:untrusted_app:s0"));
                    if(request.getString("command").equals("retire")){HostAccountAccess.retire(peer);HostRemoteServices.unavailable(peer);}
                    else{
                        JSONObject authorization=payload.getJSONObject("authorization");
                        JSONArray packages=authorization.getJSONArray("packages");HostPackages.inspectInstalled(packages.toString());
                        JSONObject selected=authorization.optJSONObject("account");
                        Account account=selected==null?null:new Account(selected.getString("name"),selected.getString("type"));
                        HostAccounts.selected(context,peer,account);
                        Set<String> visible=new HashSet<>();for(int i=0;i<packages.length();i++)visible.add(packages.getJSONObject(i).getString("packageName"));
                        HostAccountAccess.enroll(peer,account,authorization.optBoolean("networkAllowed",false),visible);
                        HostRemoteServices.expectIncoming(peer);
                    }
                    reply(id,true);
                }
                case "addAccount","removeAccount"->{
                    if(activities==null||!request.optBoolean("interactive",false)){failure(id,AccountManager.ERROR_CODE_CANCELED);break;}
                    if(!request.optBoolean("networkAllowed",false)){failure(id,AccountManager.ERROR_CODE_NETWORK_ERROR);break;}
                    String command=request.getString("command");JSONObject payload=request.getJSONObject("payload");
                    Operation operation=new Operation(id);pending.put(id,operation);
                    var service=IAccountManager.Stub.asInterface(ServiceManager.getService(Context.ACCOUNT_SERVICE));
                    if(command.equals("addAccount")){
                        String type=payload.getString("type");
                        if(Arrays.stream(accounts.getAuthenticatorTypes()).noneMatch(auth->auth.type.equals(type)))throw new IllegalArgumentException();
                        Bundle options=new Bundle();options.putString(AccountManager.KEY_ANDROID_PACKAGE_NAME,context.getPackageName());
                        service.addAccount(operation,type,null,null,true,options);
                    }else service.removeAccountAsUser(operation,new Account(payload.getString("name"),payload.getString("type")),true,context.getUserId());
                }
                default->failure(id,AccountManager.ERROR_CODE_BAD_ARGUMENTS);
            }
        }catch(SecurityException failure){diagnostic("Account authorization",failure);failure(id,AccountManager.ERROR_CODE_REMOTE_EXCEPTION,failure);}
        catch(IllegalArgumentException|JSONException failure){failure(id,AccountManager.ERROR_CODE_BAD_ARGUMENTS,failure);}
        catch(Exception failure){diagnostic("Account command",failure);failure(id,AccountManager.ERROR_CODE_REMOTE_EXCEPTION,failure);}
    }
    private JSONObject inventory()throws JSONException {
        Map<String,String> providers=new HashMap<>();JSONArray authenticators=new JSONArray();
        for(AuthenticatorDescription auth:accounts.getAuthenticatorTypes()){
            providers.put(auth.type,auth.packageName);
            authenticators.put(new JSONObject().put("providerPackage",auth.packageName).put("type",auth.type));
        }
        JSONArray values=new JSONArray();
        for(Account account:accounts.getAccounts()){
            String provider=providers.get(account.type);
            values.put(new JSONObject().put("providerPackage",provider==null?JSONObject.NULL:provider).put("type",account.type).put("name",account.name));
        }
        return new JSONObject().put("authenticators",authenticators).put("accounts",values).put("canSignIn",activities!=null);
    }
    /** A real Android response keeps the original authenticator Session alive while UI is open. */
    private final class Operation extends IAccountManagerResponse.Stub {
        final String id;AccountAuthenticatorResponse response;CompletableFuture<Void> activity;boolean done;
        Operation(String id){this.id=id;}
        @Override public void onError(int code,String message){handler.post(()->{if(finish()){if(activity!=null)activity.cancel(true);failure(id,code);}});}
        @Override public void onResult(Bundle result){handler.post(()->{
            if(done||closed.get())return;
            try{
                Intent intent=result==null?null:result.getParcelable(AccountManager.KEY_INTENT,Intent.class);
                if(intent!=null){
                    response=intent.getParcelableExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE,AccountAuthenticatorResponse.class);
                    activity=activities.launch(intent).toCompletableFuture();
                    activity.whenComplete((unused,error)->handler.post(()->{if(!done)cancel();}));
                }else if(finish()){
                    JSONObject value=inventory();if(result!=null&&result.containsKey(AccountManager.KEY_BOOLEAN_RESULT))value.put("completed",result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT));reply(id,value);
                }
            }catch(Exception failed){diagnostic("Account operation",failed);cancel();}
        });}
        boolean finish(){if(done)return false;done=true;pending.remove(id,this);return true;}
        void cancel(){if(!finish())return;HostAccounts.cancel(this);if(response!=null)response.onError(AccountManager.ERROR_CODE_CANCELED,"Account operation cancelled");if(activity!=null)activity.cancel(true);failure(id,AccountManager.ERROR_CODE_CANCELED);}
    }
    /** Exception messages may contain account data; retain only bounded type and code locations. */
    private static void diagnostic(String operation,Throwable failure){
        Set<Throwable> seen=Collections.newSetFromMap(new IdentityHashMap<>());
        for(int cause=0;failure!=null&&cause<4&&seen.add(failure);cause++,failure=failure.getCause()){
            System.err.println("[android-host] "+operation+" failed: "+failure.getClass().getName());
            StackTraceElement[] frames=failure.getStackTrace();for(int i=0;i<Math.min(frames.length,12);i++)System.err.println("[android-host] at "+frames[i]);
        }
    }
    private void reply(String id,Object result){try{send(new JSONObject().put("schemaVersion",1).put("id",id).put("result",result));}catch(JSONException invalid){failure(id,AccountManager.ERROR_CODE_INVALID_RESPONSE);}}
    private void failure(String id,int code){failure(id,code,null);}
    private void failure(String id,int code,Throwable cause){
        Operation pendingOperation=pending.remove(id);if(pendingOperation!=null&&!pendingOperation.done){pendingOperation.done=true;HostAccounts.cancel(pendingOperation);}
        System.err.println("[android-host] Account provider response code="+code);
        try{
            JSONObject error=new JSONObject().put("code",code);
            // Code identity only: exception messages can contain credentials.
            if(cause!=null){error.put("type",cause.getClass().getName());StackTraceElement[] trace=cause.getStackTrace();if(trace.length>0)error.put("site",trace[0].getClassName()+"."+trace[0].getMethodName()+":"+trace[0].getLineNumber());}
            send(new JSONObject().put("schemaVersion",1).put("id",id).put("error",error));
        }catch(JSONException impossible){throw new AssertionError(impossible);}
    }
    private void send(JSONObject value){
        if(closed.get())return;
        byte[] bytes=value.toString().getBytes(StandardCharsets.UTF_8);
        try{writer.execute(()->{
            if(closed.get())return;
            try{output.writeInt(bytes.length);output.write(bytes);output.flush();}
            catch(IOException failure){close(true);}
        });}catch(RejectedExecutionException stopped){if(!closed.get())throw stopped;}
    }
    @Override public void close(){close(false);}
    private void close(boolean notify){
        if(!closed.compareAndSet(false,true))return;
        for(var operation:pending.values())operation.cancel();pending.clear();
        if(listener!=null)try{listener.close();}catch(Exception ignored){}
        handler.removeCallbacksAndMessages(null);
        try{Os.shutdown(socket.getFileDescriptor(),OsConstants.SHUT_RDWR);}catch(Exception ignored){}
        writer.shutdownNow();
        for(var owned:new AutoCloseable[]{input,output,socket})try{owned.close();}catch(Exception ignored){}
        if(notify)handler.post(disconnected);
    }
}
