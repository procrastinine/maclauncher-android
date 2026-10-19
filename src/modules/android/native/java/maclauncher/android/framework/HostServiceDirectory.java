package maclauncher.android.framework;

import android.content.Context;
import android.os.*;
import java.io.IOException;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.CompletableFuture;

/** Exposes existing process services through complete original AIDL dispatchers. */
public final class HostServiceDirectory implements AutoCloseable,InvocationHandler {
    private static final String PROCESS_DIRECTORY="maclauncher.android.ProcessDirectory";
    private static final int PUBLISH_DIRECTORY=IBinder.LAST_CALL_TRANSACTION;
    private final Context context;
    private final IServiceManager manager;
    private final HostProcessServices processes;
    private final Map<CallbackKey,Notification> callbacks=new HashMap<>();
    private boolean closed;
    private record CallbackKey(String name,IBinder binder){}
    private final class Notification extends IServiceCallback.Stub implements IBinder.DeathRecipient {
        final CallbackKey key;final IServiceCallback callback;final IServiceManager manager;
        Notification(IServiceManager manager,String name,IServiceCallback callback){this.manager=manager;key=new CallbackKey(name,callback.asBinder());this.callback=callback;}
        @Override public void onRegistration(String name,IBinder service)throws RemoteException{try{callback.onRegistration(name,export(service));}catch(RemoteException gone){retire(this);throw gone;}}
        @Override public void binderDied(){try{retire(this);}catch(RemoteException ignored){}}
    }
    public HostServiceDirectory(Context context){this(context,false);}
    public HostServiceDirectory(Context context,boolean publishedOnly){this(context,publishedOnly,null);}
    public HostServiceDirectory(Context context,boolean publishedOnly,HostProcessServices processes){this.context=Objects.requireNonNull(context);this.processes=processes;manager=publishedOnly?HostServices.publishedManager():HostServices.manager();}
    public IBinder root(){
        synchronized(this){if(closed)throw new IllegalStateException("Android service directory is closed");}
        IServiceManager facade=(IServiceManager)Proxy.newProxyInstance(IServiceManager.class.getClassLoader(),new Class<?>[]{IServiceManager.class},this);
        IBinder original=HostServiceEndpoints.export(facade,IServiceManager.DESCRIPTOR,context);
        if(processes==null)return original;
        return new Binder(){
            {attachInterface(null,IServiceManager.DESCRIPTOR);}
            @Override protected boolean onTransact(int code,Parcel request,Parcel reply,int flags)throws RemoteException{
                if(code!=PUBLISH_DIRECTORY)return original.transact(code,request,reply,flags);
                request.enforceInterface(PROCESS_DIRECTORY);IBinder remote=request.readStrongBinder();request.enforceNoDataAvail();
                HostBinderRpc.Peer peer=HostBinderRpc.callingPeer();if(peer==null)throw new SecurityException("Android process directory requires an authenticated connection");
                processes.attachDirectory(peer,remote);reply.writeNoException();return true;
            }
        };
    }
    static void publish(IBinder remote,IBinder local)throws RemoteException{
        Parcel request=Parcel.obtain(remote),reply=Parcel.obtain();
        try{request.writeInterfaceToken(PROCESS_DIRECTORY);request.writeStrongBinder(local);if(!remote.transact(PUBLISH_DIRECTORY,request,reply,0))throw new RemoteException("Android process directory publication is unavailable");reply.readException();}
        finally{request.recycle();reply.recycle();}
    }
    /** Publish both original directories on the same authenticated RPC session. */
    public static void connect(ParcelFileDescriptor socket,HostBinderRpc.Peer peer,Context context,int threads,boolean server)throws IOException,RemoteException{
        connect(socket,peer,context,threads,server,null);
    }
    public static void connect(ParcelFileDescriptor socket,HostBinderRpc.Peer peer,Context context,int threads,boolean server,String connectionId)throws IOException,RemoteException{
        final String descriptor="maclauncher.android.ProcessDirectory";
        HostServiceDirectory directory=new HostServiceDirectory(context);IBinder local=directory.root();
        HostBinderRpc.Connection connection=null;
        try{
            if(server){
                CompletableFuture<HostBinderRpc.Connection> owner=new CompletableFuture<>();
                Binder exchange=new Binder(){
                    {attachInterface(null,descriptor);}
                    @Override protected boolean onTransact(int code,Parcel request,Parcel reply,int flags)throws RemoteException{
                        if(code!=FIRST_CALL_TRANSACTION)return super.onTransact(code,request,reply,flags);
                        request.enforceInterface(descriptor);IBinder remote=request.readStrongBinder();request.enforceNoDataAvail();
                        HostRemoteServices.attach(owner.join(),remote,connectionId);reply.writeNoException();reply.writeStrongBinder(local);return true;
                    }
                };
                connection=HostBinderRpc.serve(socket,peer,exchange,threads);owner.complete(connection);HostRemoteServices.stage(connection,connectionId);
            }else{
                connection=HostBinderRpc.connect(socket,peer,threads);HostRemoteServices.stage(connection,connectionId);IBinder exchange=connection.root();
                Parcel request=Parcel.obtain(exchange),reply=Parcel.obtain();
                try{request.writeInterfaceToken(descriptor);request.writeStrongBinder(local);if(!exchange.transact(IBinder.FIRST_CALL_TRANSACTION,request,reply,0))throw new RemoteException("Android process directory exchange is unavailable");reply.readException();HostRemoteServices.attach(connection,reply.readStrongBinder(),connectionId);}
                finally{request.recycle();reply.recycle();}
            }
            connection.closed().whenComplete((unused,failure)->directory.close());
        }catch(IOException|RemoteException|RuntimeException|Error failure){if(connection!=null)connection.close();directory.close();throw failure;}
    }
    @Override public Object invoke(Object proxy,Method method,Object[] args)throws Throwable{
        if(method.getDeclaringClass()==Object.class)return switch(method.getName()){case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==args[0];case "toString"->"Android process service directory";default->throw new AssertionError(method);};
        // This facade owns export conversion and notification lifetimes. Its
        // generated AIDL endpoint must not be bypassed by the manager's binder.
        if(method.getName().equals("asBinder"))return null;
        synchronized(this){if(closed)throw new DeadObjectException();}
        HostBinderRpc.Peer peer=HostBinderRpc.callingPeer();
        IServiceManager manager=processes!=null&&peer!=null?processes.serviceDirectory(peer):this.manager;
        if(method.getName().equals("registerForNotifications")){
            String name=(String)args[0];IServiceCallback callback=(IServiceCallback)args[1];CallbackKey key=new CallbackKey(name,callback.asBinder());Notification owned;
            synchronized(this){owned=callbacks.get(key);if(owned==null){owned=new Notification(manager,name,callback);key.binder.linkToDeath(owned,0);callbacks.put(key,owned);}}
            try{manager.registerForNotifications(name,owned);}catch(RemoteException|RuntimeException failure){retire(owned);throw failure;}return null;
        }
        if(method.getName().equals("unregisterForNotifications")){
            Notification owned;synchronized(this){owned=callbacks.get(new CallbackKey((String)args[0],((IServiceCallback)args[1]).asBinder()));}
            if(owned!=null)retire(owned);return null;
        }
        Object value;try{value=method.invoke(manager,args);}catch(InvocationTargetException failure){throw failure.getCause();}
        if(value instanceof IBinder binder)return export(binder);
        if(value instanceof android.os.Service service){
            if(service.getTag()==android.os.Service.accessor)return android.os.Service.accessor(export(service.getAccessor()));
            ServiceWithMetadata original=service.getServiceWithMetadata();ServiceWithMetadata copy=new ServiceWithMetadata();copy.service=export(original.service);copy.isLazyService=original.isLazyService;return android.os.Service.serviceWithMetadata(copy);
        }
        return value;
    }
    private IBinder export(IBinder binder)throws RemoteException{
        if(binder==null)return null;String descriptor=binder.getInterfaceDescriptor();IInterface target=descriptor==null?null:binder.queryLocalInterface(descriptor);
        return target==null?binder:HostServiceEndpoints.export(target,descriptor,context);
    }
    private void retire(Notification notification)throws RemoteException{
        synchronized(this){if(!callbacks.remove(notification.key,notification))return;}
        try{notification.key.binder.unlinkToDeath(notification,0);}catch(NoSuchElementException ignored){}
        notification.manager.unregisterForNotifications(notification.key.name,notification);
    }
    @Override public void close(){List<Notification> snapshot;synchronized(this){if(closed)return;closed=true;snapshot=List.copyOf(callbacks.values());}for(Notification notification:snapshot)try{retire(notification);}catch(RemoteException gone){}}
}
