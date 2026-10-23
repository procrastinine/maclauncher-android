package maclauncher.android.framework;

import android.app.*;
import android.app.Service;
import android.content.*;
import android.content.pm.*;
import android.os.*;
import java.util.*;
import java.util.concurrent.Executor;

/** Local service-process ownership below the original Service and LoadedApk dispatchers. */
public final class HostLocalServices {
    private static Owner current;
    private HostLocalServices(){}
    private static synchronized Owner owner(){if(current==null)current=new Owner();return current;}
    private static ServiceInfo resolve(Context context,Intent intent){
        Objects.requireNonNull(intent);ResolveInfo resolved=context.getPackageManager().resolveService(intent,PackageManager.GET_META_DATA);return resolved==null?null:resolved.serviceInfo;
    }
    private static boolean local(ServiceInfo info){return HostPackages.owns(info);}
    private static Intent explicit(Intent intent,ServiceInfo info){return new Intent(intent).setComponent(new ComponentName(info.packageName,info.name));}
    public static ComponentName start(Context context,Intent intent){ServiceInfo info=resolve(context,intent);if(info==null)return null;Intent selected=explicit(intent,info);try{return local(info)?owner().start(selected.getComponent(),selected):HostRemoteServices.start(context,info,selected);}catch(RemoteException failure){throw failure.rethrowFromSystemServer();}}
    public static boolean stop(Context context,Intent intent){ServiceInfo info=resolve(context,intent);if(info==null)return false;Intent selected=explicit(intent,info);try{if(!local(info))return HostRemoteServices.stop(context,info,selected);Owner owner;synchronized(HostLocalServices.class){owner=current;}return owner!=null&&owner.stop(selected.getComponent(),null,-1);}catch(RemoteException failure){throw failure.rethrowFromSystemServer();}}
    public static boolean bind(Context context,Intent intent,IServiceConnection connection,long flags){ServiceInfo info=resolve(context,intent);if(info==null)return false;Intent selected=explicit(intent,info);try{return local(info)?owner().bind(selected.getComponent(),selected,Objects.requireNonNull(connection),flags):HostRemoteServices.bind(context,info,selected,connection,flags);}catch(RemoteException failure){throw failure.rethrowFromSystemServer();}}
    public static boolean unbind(IServiceConnection connection){try{boolean remote=HostRemoteServices.unbind(connection);Owner owner;synchronized(HostLocalServices.class){owner=current;}if(owner==null)return remote;owner.unbind(Objects.requireNonNull(connection));return true;}catch(RemoteException failure){throw failure.rethrowFromSystemServer();}}
    public static boolean stopToken(ComponentName name,IBinder token,int id){Owner owner;synchronized(HostLocalServices.class){owner=current;}return owner!=null&&owner.stop(name,token,id);}
    public static void foreground(ComponentName name,IBinder token,int id,Notification notification,int flags,int type){Owner owner;synchronized(HostLocalServices.class){owner=current;}if(owner!=null)owner.foreground(name,token,id,notification,flags,type);}
    public static int foregroundType(ComponentName name,IBinder token){Owner owner;synchronized(HostLocalServices.class){owner=current;}if(owner==null)return 0;synchronized(owner){Entry entry=owner.entries.get(name);return entry!=null&&entry.token==token?entry.foregroundType:0;}}
    /** Android's ordinary query observes this process's existing services only. */
    public static List<ActivityManager.RunningServiceInfo> running(int max){Owner owner;synchronized(HostLocalServices.class){owner=current;}return owner==null||max<=0?new ArrayList<>():owner.running(max);}
    public static void shutdown(){Owner owner;synchronized(HostLocalServices.class){owner=current;current=null;}if(owner!=null)owner.close();}
    private static final class Group {final Intent intent;final Map<IBinder,IServiceConnection> clients=new LinkedHashMap<>();IBinder binder;boolean bound,rebind;Group(Intent intent){this.intent=intent;}}
    private static final class Entry {final ComponentName name;final IBinder token=new Binder();final LinkedHashMap<Intent.FilterComparison,Group> groups=new LinkedHashMap<>();final long activeSince=SystemClock.elapsedRealtime();long lastActivityTime=SystemClock.uptimeMillis();Service service;boolean started;int startId;volatile int foregroundId,foregroundType;Entry(ComponentName name){this.name=name;}}
    private static final class Owner {
        Application application;final Handler handler=new Handler(Looper.getMainLooper());final Object messages=new Object();final Map<ComponentName,Entry> entries=new LinkedHashMap<>();final Map<IBinder,IBinder.DeathRecipient> deaths=new HashMap<>();int pending;boolean closed;
        private boolean post(Runnable operation){if(closed)return false;pending++;if(handler.postAtTime(()->{try{operation.run();}finally{synchronized(this){pending--;}}},messages,SystemClock.uptimeMillis()))return true;pending--;return false;}
        private Entry reserve(ComponentName name){Entry entry=entries.get(name);if(entry!=null)return entry;entry=new Entry(name);entries.put(name,entry);return entry;}
        synchronized ComponentName start(ComponentName name,Intent intent){if(closed)return null;Entry entry=reserve(name);entry.started=true;entry.lastActivityTime=SystemClock.uptimeMillis();int id=++entry.startId;if(!post(()->{if(create(entry,intent))entry.service.onStartCommand(intent,0,id);})){entry.started=false;return null;}return name;}
        synchronized boolean stop(ComponentName name,IBinder token,int id){Entry entry=entries.get(name);if(entry==null||token!=null&&entry.token!=token||id>=0&&entry.startId!=id)return false;entry.started=false;return post(()->retireUnused(entry));}
        synchronized boolean bind(ComponentName name,Intent intent,IServiceConnection connection,long flags){if(closed)return false;Entry entry=entries.get(name);if(entry==null&&(flags&Context.BIND_AUTO_CREATE)==0)return false;entry=reserve(name);entry.lastActivityTime=SystemClock.uptimeMillis();Intent.FilterComparison key=new Intent.FilterComparison(intent);Group group=entry.groups.get(key);if(group==null){group=new Group(intent);entry.groups.put(key,group);}IBinder client=connection.asBinder();if(group.clients.putIfAbsent(client,connection)!=null)return true;if(!deaths.containsKey(client)){IBinder.DeathRecipient death=()->unbind(connection);try{client.linkToDeath(death,0);deaths.put(client,death);}catch(RemoteException gone){group.clients.remove(client);return false;}}Entry bound=entry;Group binding=group;
            return post(()->{if(!create(bound,binding.intent))return;synchronized(this){if(!binding.clients.containsKey(client))return;}if(!binding.bound){binding.binder=HostGoogleProviders.serviceBinder(bound.service,bound.service.onBind(binding.intent));binding.bound=true;}else if(binding.rebind){binding.rebind=false;bound.service.onRebind(binding.intent);}try{connection.connected(name,binding.binder,false);}catch(RemoteException failure){unbind(connection);}});
        }
        synchronized void unbind(IServiceConnection connection){IBinder client=connection.asBinder();IBinder.DeathRecipient death=deaths.remove(client);if(death!=null)try{client.unlinkToDeath(death,0);}catch(NoSuchElementException gone){}for(Entry entry:entries.values())for(Group group:entry.groups.values())if(group.clients.remove(client)!=null&&group.clients.isEmpty())post(()->{synchronized(this){if(!group.clients.isEmpty())return;}if(entry.service!=null&&group.bound)group.rebind=entry.service.onUnbind(group.intent);retireUnused(entry);});}
        private boolean create(Entry entry,Intent intent){synchronized(this){if(closed||entries.get(entry.name)!=entry)return false;if(entry.service!=null)return true;}
            Service service=null;try{Application selected=HostApplication.serviceApplication(entry.name);service=HostApplication.instantiateService(entry.name.getClassName(),intent);HostApplication.attachService(service,selected,entry.token,entry.name.getClassName());synchronized(this){application=selected;entry.service=service;}service.onCreate();return true;}
            catch(Throwable failure){synchronized(this){entries.remove(entry.name,entry);}if(service!=null)try{service.detachAndCleanUp();}catch(Throwable cleanup){if(cleanup!=failure)failure.addSuppressed(cleanup);}return HostUiAccess.raise(failure instanceof java.lang.reflect.InvocationTargetException wrapped?wrapped.getCause():failure);}
        }
        private void retireUnused(Entry entry){synchronized(this){if(entry.started)return;for(Group group:entry.groups.values())if(!group.clients.isEmpty())return;if(!entries.remove(entry.name,entry))return;}destroy(entry);}
        synchronized List<ActivityManager.RunningServiceInfo> running(int max){
            var result=new ArrayList<ActivityManager.RunningServiceInfo>();
            if(closed)return result;
            for(Entry entry:entries.values()){
                if(entry.service==null)continue;
                var info=new ActivityManager.RunningServiceInfo();info.service=entry.name;info.pid=android.os.Process.myPid();info.uid=application.getApplicationInfo().uid;info.process=HostContext.hostProcessName();
                info.activeSince=entry.activeSince;info.lastActivityTime=entry.lastActivityTime;info.started=entry.started;info.foreground=entry.foregroundId!=0;
                var clients=new HashSet<IBinder>();for(Group group:entry.groups.values())clients.addAll(group.clients.keySet());info.clientCount=clients.size();
                info.flags=(info.started?ActivityManager.RunningServiceInfo.FLAG_STARTED:0)|(info.foreground?ActivityManager.RunningServiceInfo.FLAG_FOREGROUND:0);
                if(info.uid==android.os.Process.SYSTEM_UID)info.flags|=ActivityManager.RunningServiceInfo.FLAG_SYSTEM_PROCESS;
                if((application.getApplicationInfo().flags&ApplicationInfo.FLAG_PERSISTENT)!=0)info.flags|=ActivityManager.RunningServiceInfo.FLAG_PERSISTENT_PROCESS;
                result.add(info);if(result.size()>=max)break;
            }
            return result;
        }
        private void destroy(Entry entry){if(entry.service==null)return;Throwable first=null;try{entry.service.onDestroy();}catch(Throwable failure){first=failure;}finally{try{HostApplication.releaseContext(entry.service.getBaseContext(),entry.name.getClassName());entry.service.detachAndCleanUp();}catch(Throwable cleanup){if(first==null)first=cleanup;else if(first!=cleanup)first.addSuppressed(cleanup);}entry.service=null;entry.groups.clear();if(entry.foregroundId!=0)application.getSystemService(NotificationManager.class).cancel(entry.foregroundId);}if(first!=null)HostUiAccess.raise(first);}
        synchronized void foreground(ComponentName name,IBinder token,int id,Notification notification,int flags,int type){Entry entry=entries.get(name);if(entry==null||entry.token!=token)return;post(()->{NotificationManager manager=application.getSystemService(NotificationManager.class);if(id!=0&&notification!=null){if(entry.foregroundId!=0&&entry.foregroundId!=id)manager.cancel(entry.foregroundId);entry.foregroundId=id;entry.foregroundType=type;manager.notify(id,notification);}else{if((flags&Service.STOP_FOREGROUND_REMOVE)!=0&&entry.foregroundId!=0)manager.cancel(entry.foregroundId);entry.foregroundId=0;entry.foregroundType=0;}});}
        void close(){if(!handler.getLooper().isCurrentThread())throw new IllegalStateException("Services retire on the application owner thread");List<Entry> snapshot;synchronized(this){if(closed)return;closed=true;handler.removeCallbacksAndMessages(messages);snapshot=List.copyOf(entries.values());entries.clear();for(var death:deaths.entrySet())try{death.getKey().unlinkToDeath(death.getValue(),0);}catch(NoSuchElementException gone){}deaths.clear();}Throwable first=null;for(Entry entry:snapshot)try{destroy(entry);}catch(Throwable failure){if(first==null)first=failure;else if(first!=failure)first.addSuppressed(failure);}if(first!=null)HostUiAccess.raise(first);}
    }
}
