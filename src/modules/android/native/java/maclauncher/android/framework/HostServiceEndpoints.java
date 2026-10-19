package maclauncher.android.framework;

import android.content.Context;
import android.os.*;
import java.lang.reflect.InvocationTargetException;
import java.lang.ref.WeakReference;
import java.util.*;

/** Keep original AIDL parcel dispatch when local platform services cross a process. */
public final class HostServiceEndpoints {
    private HostServiceEndpoints(){}
    private static final Map<IInterface,WeakReference<IBinder>> endpoints=new WeakHashMap<>();
    public static synchronized IBinder export(IInterface target,String descriptor,Context context){
        Objects.requireNonNull(target);Objects.requireNonNull(descriptor);Objects.requireNonNull(context);
        if(target instanceof HostServiceClients.Target client)target=client.platformTarget();
        try{
            IBinder supplied=target.asBinder();
            if(supplied!=null&&hasTransactionOwner(supplied))return supplied;
            Class<?> contract=Class.forName(descriptor,false,HostServiceEndpoints.class.getClassLoader());
            WeakReference<IBinder> previous=endpoints.get(target);IBinder result=previous==null?null:previous.get();if(result!=null)return result;
            Class<?> implementation=Class.forName("maclauncher.android.framework.endpoints."+descriptor,false,HostServiceEndpoints.class.getClassLoader());
            result=(IBinder)implementation.getConstructor(contract,PermissionEnforcer.class).newInstance(target,PermissionEnforcer.fromContext(context));
            endpoints.put(target,new WeakReference<>(result));return result;
        }catch(InvocationTargetException failure){return HostUiAccess.raise(failure.getCause());}
        catch(ReflectiveOperationException failure){throw new IllegalStateException("Android service dispatch is unavailable: "+descriptor,failure);}
    }
    private static boolean hasTransactionOwner(IBinder supplied){
        if(!(supplied instanceof Binder))return true;
        for(Class<?> owner=supplied.getClass();owner!=Binder.class;owner=owner.getSuperclass()){
            try{owner.getDeclaredMethod("onTransact",int.class,Parcel.class,Parcel.class,int.class);return true;}
            catch(NoSuchMethodException inherited){}
        }
        return false;
    }
}
