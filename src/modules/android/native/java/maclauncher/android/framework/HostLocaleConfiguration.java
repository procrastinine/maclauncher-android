package maclauncher.android.framework;

import android.app.ILocaleManager;
import android.content.Context;
import android.os.*;
import com.android.server.wm.HostPackageConfiguration;
import java.util.*;
import java.util.concurrent.*;

/** Authenticated configuration delivery through the existing process directory. */
public final class HostLocaleConfiguration {
    private static final String NAME="maclauncher.package_configuration";
    private static final String DESCRIPTOR="android.os.IRemoteCallback";
    private static Client client;
    private static Server server;
    private HostLocaleConfiguration(){}
    public static synchronized void install(Context context){
        if(android.os.Process.myUid()==android.os.Process.SYSTEM_UID||client!=null)return;
        try{
            if(!HostServices.manager().isDeclared(Context.LOCALE_SERVICE))return;
            IBinder binder=ServiceManager.getService(Context.LOCALE_SERVICE);
            if(binder==null)throw new IllegalStateException("Android locale service is unavailable");
            var manager=ILocaleManager.Stub.asInterface(binder);
            Client owner=new Client(context,HostBinderRpc.ownerOf(binder));
            // This runs before AppComponentFactory and Application constructors.
            HostApplication.applicationLocalesChanged(manager.getApplicationLocales(context.getPackageName(),context.getUserId()));
            HostServices.register(NAME,owner,DESCRIPTOR);client=owner;
        }catch(RemoteException failure){throw failure.rethrowFromSystemServer();}
    }
    public static synchronized void retire(){if(client!=null){Client owner=client;client=null;try{HostServices.unregister(NAME,owner,DESCRIPTOR);}finally{owner.close();}}}
    private static final class Client extends IRemoteCallback.Stub {
        final Context context;final HostBinderRpc.Peer owner;final Handler handler;String generation;long sequence=-1;boolean closed;
        Client(Context context,HostBinderRpc.Peer owner){this.context=context;handler=new Handler(context.getMainLooper());this.owner=Objects.requireNonNull(owner,"Android locale service requires an authenticated system process");if(owner.uid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("Android locale service is not system-owned");}
        synchronized void close(){closed=true;handler.removeCallbacksAndMessages(this);}
        @Override public void sendResult(Bundle result){
            var peer=HostBinderRpc.callingPeer();
            if(!owner.equals(peer)||peer.uid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("Android configuration sender is not its system owner");
            Objects.requireNonNull(result);String name=result.getString("package"),id=result.getString("generation");int user=result.getInt("user",-1);long revision=result.getLong("sequence",-1);LocaleList locales=result.getParcelable("locales",LocaleList.class);
            if(!context.getPackageName().equals(name)||context.getUserId()!=user||id==null||id.length()!=36||revision<0||locales==null)throw new IllegalArgumentException("Android configuration identity is invalid");
            synchronized(this){if(closed)return;if(generation!=null&&!generation.equals(id))throw new SecurityException("Android configuration owner changed");generation=id;if(revision<=sequence)return;sequence=revision;}
            synchronized(this){if(closed)return;if(!handler.postAtTime(()->{synchronized(this){if(closed||revision!=sequence)return;}HostApplication.applicationLocalesChanged(locales);},this,SystemClock.uptimeMillis()))throw new IllegalStateException("Android configuration Looper has stopped");}
        }
    }
    public static AutoCloseable packages(com.android.server.locales.HostLocaleService service){
        var observer=new android.content.pm.PackageManagerInternal.PackageListObserver(){
            public void onPackageAdded(String name,int uid){service.packageAdded(name,uid);}
            public void onPackageRemoved(String name,int uid){service.packageRemoved(name,uid);}
            public void onPackageChanged(String name,int uid){service.packageChanged(name,uid);}
        };
        HostPackages.addPackageObserver(observer);return ()->HostPackages.removePackageObserver(observer);
    }
    public static synchronized AutoCloseable remote(HostProcessServices processes){if(server!=null)throw new IllegalStateException("Android locale directories already have an owner");server=new Server(processes);return server;}
    public static void changed(String name,int user){Server owner;synchronized(HostLocaleConfiguration.class){owner=server;}if(owner!=null)owner.changed(name,user);}
    private static final class Server implements AutoCloseable {
        final String generation=UUID.randomUUID().toString();
        final Map<HostProcessServices.Process,HostProcessServices.Directory> directories=new LinkedHashMap<>();
        final ExecutorService delivery=Executors.newSingleThreadExecutor(task->{Thread thread=new Thread(task,"Android locale configuration");thread.setDaemon(true);return thread;});
        final AutoCloseable observation;long sequence;boolean closed;
        Server(HostProcessServices processes){try{observation=processes.observeDirectories(this::directory);}catch(RuntimeException|Error failure){delivery.shutdownNow();throw failure;}}
        void directory(HostProcessServices.Directory directory){
            synchronized(this){if(closed)return;if(!directory.available()){directories.remove(directory.process());return;}if(directory.peer().uid()==android.os.Process.SYSTEM_UID)return;directories.put(directory.process(),directory);}
            enqueue(directory);
        }
        void changed(String name,int user){List<HostProcessServices.Directory> snapshot;synchronized(this){if(closed)return;sequence++;snapshot=directories.values().stream().filter(d->d.peer().packageName().equals(name)&&UserHandle.getUserId(d.peer().uid())==user).toList();}for(var d:snapshot)enqueue(d);}
        void enqueue(HostProcessServices.Directory directory){synchronized(this){if(closed)return;delivery.execute(()->send(directory));}}
        void send(HostProcessServices.Directory directory){
            try{
                long revision;synchronized(this){if(closed||directories.get(directory.process())!=directory)return;revision=sequence;}
                var peer=directory.process().identity();if(!peer.equals(directory.peer()))return;
                var config=HostPackageConfiguration.get(null,peer.packageName(),UserHandle.getUserId(peer.uid()));
                LocaleList locales=config==null||config.mLocales==null?LocaleList.getEmptyLocaleList():config.mLocales;
                IBinder binder=directory.process().service(NAME);if(binder==null)return;
                Bundle result=new Bundle();result.putString("package",peer.packageName());result.putInt("user",UserHandle.getUserId(peer.uid()));result.putString("generation",generation);result.putLong("sequence",revision);result.putParcelable("locales",locales);
                synchronized(this){if(closed||directories.get(directory.process())!=directory)return;}
                IRemoteCallback.Stub.asInterface(binder).sendResult(result);
            }catch(RemoteException|IllegalStateException retired){/* The same process-directory lifetime owns connection retirement. */}
        }
        @Override public void close()throws Exception{synchronized(this){if(closed)return;closed=true;directories.clear();}try{observation.close();}finally{delivery.shutdownNow();synchronized(HostLocaleConfiguration.class){if(server==this)server=null;}}}
    }
}
