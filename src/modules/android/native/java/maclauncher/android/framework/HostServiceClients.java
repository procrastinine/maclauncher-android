package maclauncher.android.framework;

import android.os.*;
import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.util.*;

/** The process hop from an Android system-service client to a local platform owner.
 * Incoming application Binder calls and exported service dispatch remain unchanged. */
public final class HostServiceClients {
    private HostServiceClients(){}
    /** Implemented by generated, typed delegates for every supplied AIDL contract. */
    public interface Target { IInterface platformTarget(); }
    private static final Map<IInterface,WeakReference<Boundary>> clients=new WeakHashMap<>();

    public static IInterface local(IInterface target,String descriptor){
        if(target==null||target instanceof Target)return target;
        IBinder supplied=target.asBinder();
        if(supplied!=null&&supplied.queryLocalInterface(descriptor)==null)return target;
        return boundary(target,descriptor,supplied).client;
    }
    private static Boundary boundary(IInterface target,String descriptor,IBinder supplied){
        synchronized(clients){
            var previous=clients.get(target);Boundary value=previous==null?null:previous.get();
            if(value!=null)return value;
            value=new Boundary(target,descriptor,supplied);clients.put(target,new WeakReference<>(value));return value;
        }
    }
    public static IBinder binder(IBinder source)throws RemoteException{
        if(source==null||source instanceof Boundary)return source;
        String descriptor=source.getInterfaceDescriptor();IInterface target=descriptor==null?null:source.queryLocalInterface(descriptor);
        // Application-defined Binder contracts keep their own local-call rules.
        // The generated platform surface is the complete supplied framework.
        if(target!=null)try{Class.forName(descriptor,false,HostServiceClients.class.getClassLoader());}
        catch(ClassNotFoundException applicationContract){return source;}
        return target==null?source:boundary(target,descriptor,source);
    }
    /** AIDL service handles returned by a system service retain the same hop.
     * Parcelled application callbacks/providers are not replaced. */
    public static Object result(Object value,Class<?> declared)throws RemoteException{
        if(value instanceof IInterface target&&IInterface.class.isAssignableFrom(declared))return local(target,declared.getName());
        if(value instanceof IBinder source)return binder(source);
        if(value instanceof android.os.Service service){
            if(service.getTag()==android.os.Service.accessor)return android.os.Service.accessor(binder(service.getAccessor()));
            ServiceWithMetadata original=service.getServiceWithMetadata(),copy=new ServiceWithMetadata();
            copy.service=binder(original.service);copy.isLazyService=original.isLazyService;return android.os.Service.serviceWithMetadata(copy);
        }
        return value;
    }
    /** Passing a platform handle back to its local owner preserves its original
     * object and Binder identity, just as unmarshalling a local Binder would. */
    public static Object argument(Object value){
        if(value instanceof Target client)return client.platformTarget();
        if(value instanceof Boundary client)return client.supplied!=null?client.supplied:HostServiceEndpoints.export(client.target,client.descriptor,HostApplication.applicationContext());
        return value;
    }
    private static final class Boundary extends Binder {
        final IInterface target,client;final String descriptor;final IBinder supplied;
        Boundary(IInterface target,String descriptor,IBinder supplied){
            this.target=target;this.descriptor=descriptor;this.supplied=supplied;
            try{
                Class<?> contract=Class.forName(descriptor,false,HostServiceClients.class.getClassLoader());
                Class<?> implementation=Class.forName("maclauncher.android.framework.clients."+descriptor,false,HostServiceClients.class.getClassLoader());
                // A typed client is an invocation boundary, not a new Binder
                // capability. Parcel transfers must retain the original owner,
                // so an exported handle returning to this process resolves to
                // its original object through normal AIDL unmarshalling.
                client=(IInterface)implementation.getConstructor(contract,IBinder.class).newInstance(target,supplied!=null?supplied:this);
                attachInterface(client,descriptor);
            }catch(InvocationTargetException failure){throw HostUiAccess.<RuntimeException,RuntimeException>raise(failure.getCause());}
            catch(ReflectiveOperationException failure){throw new IllegalStateException("Android system-service client is unavailable: "+descriptor,failure);}
        }
        // A handle transferred to another process uses original AIDL dispatch,
        // preserving the authenticated remote caller instead of its local facade.
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            return HostServiceEndpoints.export(target,descriptor,HostApplication.applicationContext()).transact(code,data,reply,flags);
        }
        @Override public boolean isBinderAlive(){return supplied==null||supplied.isBinderAlive();}
        @Override public boolean pingBinder(){return supplied==null||supplied.pingBinder();}
        @Override public void linkToDeath(DeathRecipient recipient,int flags){try{if(supplied!=null)supplied.linkToDeath(recipient,flags);else super.linkToDeath(recipient,flags);}catch(RemoteException failure){throw HostUiAccess.<RuntimeException,RuntimeException>raise(failure);}}
        @Override public boolean unlinkToDeath(DeathRecipient recipient,int flags){return supplied!=null?supplied.unlinkToDeath(recipient,flags):super.unlinkToDeath(recipient,flags);}
    }
}
