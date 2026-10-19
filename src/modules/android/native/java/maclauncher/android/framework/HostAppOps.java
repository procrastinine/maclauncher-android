package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.os.*;
import com.android.internal.app.*;
import java.lang.reflect.*;
import java.util.*;
import maclauncher.android.location.ManualLocation;
import maclauncher.android.media.MediaInputs;

/** Process-local app-operation service over the existing explicit permission owners. */
public final class HostAppOps {
    private HostAppOps() {}
    private static final Binder binder=new Binder();
    private static final IAppOpsService service=(IAppOpsService)Proxy.newProxyInstance(IAppOpsService.class.getClassLoader(),new Class<?>[]{IAppOpsService.class},HostAppOps::invoke);
    private record Watch(int op,String packageName) {}
    private record Active(IBinder token,int op,int uid,String packageName,String attribution) {}
    /** One attributed start owns its client's death link until its final finish. */
    private static final class Started implements IBinder.DeathRecipient {
        final Active key;int count;boolean retired;
        Started(Active key){this.key=key;}
        @Override public void binderDied(){synchronized(HostAppOps.class){retired=true;active.remove(key,this);}}
        void unlink(){
            if(retired)return;retired=true;
            try{key.token.unlinkToDeath(this,0);}catch(NoSuchElementException alreadyUnlinked){}
        }
    }
    private static final RemoteCallbackList<IAppOpsCallback> watchers=new RemoteCallbackList<>();
    private static final Map<Active,Started> active=new HashMap<>();
    private static final Map<IBinder,Set<Integer>> globalRestrictions=new IdentityHashMap<>();
    private static final Set<String> diagnostics=new HashSet<>();
    private static Handler handler;private static String packageName;private static int uid;private static boolean installed,closed,pending;
    static {binder.attachInterface(service,"com.android.internal.app.IAppOpsService");}
    public static synchronized void install(Context context){
        if(closed)throw new IllegalStateException("Application operation service is closed");
        if(installed)return;packageName=context.getPackageName();uid=android.os.Process.myUid();handler=new Handler(Objects.requireNonNull(Looper.getMainLooper()));
        HostServices.register(Context.APP_OPS_SERVICE,service,"com.android.internal.app.IAppOpsService");
        if(!HostLooper.addShutdownListener(handler.getLooper(),HostAppOps::close))throw new IllegalStateException("Application operation owner has stopped");installed=true;
    }
    public static AppOpsManager create(Context context){install(context);try{Constructor<AppOpsManager> constructor=AppOpsManager.class.getDeclaredConstructor(Context.class,IAppOpsService.class);constructor.setAccessible(true);return constructor.newInstance(context,service);}catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}}
    private static synchronized boolean own(int requestUid,String requestPackage){return !closed&&installed&&requestUid==uid&&Objects.equals(packageName,requestPackage);}
    private static void unavailable(String operation){synchronized(diagnostics){if(diagnostics.size()>=32||!diagnostics.add(operation))return;}System.err.println("[android-host] Application operation unavailable: "+operation);}
    private static int mode(int op,int requestUid,String requestPackage){
        if(!own(requestUid,requestPackage))return AppOpsManager.MODE_ERRORED;
        synchronized(HostAppOps.class){if(globalRestrictions.values().stream().anyMatch(ops->ops.contains(op)))return AppOpsManager.MODE_IGNORED;}
        if(op==AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE)return HostStorageSettings.mode();
        String permission;try{permission=AppOpsManager.opToPermission(op);}catch(IndexOutOfBoundsException|IllegalArgumentException error){return AppOpsManager.MODE_ERRORED;}
        if(ManualLocation.isLocationPermission(permission))return ManualLocation.isGranted()?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_IGNORED;
        if(MediaInputs.isMediaPermission(permission))return MediaInputs.isGranted(permission)?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_IGNORED;
        if(permission!=null)return HostPermissions.check(permission,requestUid,Context.DEVICE_ID_DEFAULT)==android.content.pm.PackageManager.PERMISSION_GRANTED?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_IGNORED;
        return AppOpsManager.opToDefaultMode(op);
    }
    /** Same policy owner for original system-service AppOps restrictions. */
    public static void hostSetGlobalRestriction(Object receiver,int op,boolean restricted,IBinder token){
        if(receiver instanceof AppOpsManagerInternal original){original.setGlobalRestriction(op,restricted,token);return;}
        if(android.os.Process.myUid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("Global application-operation policy requires the system owner");
        Objects.requireNonNull(token);AppOpsManager.opToName(op);
        synchronized(HostAppOps.class){
            if(restricted)globalRestrictions.computeIfAbsent(token,key->new HashSet<>()).add(op);
            else{var ops=globalRestrictions.get(token);if(ops!=null){ops.remove(op);if(ops.isEmpty())globalRestrictions.remove(token);}}
        }
        permissionsChanged();
    }
    static synchronized void retireGlobalRestrictions(IBinder token){globalRestrictions.remove(token);}
    private static SyncNotedAppOp note(int op,int requestUid,String requestPackage,String attribution){return new SyncNotedAppOp(mode(op,requestUid,requestPackage),op,attribution,requestPackage);}
    private static SyncNotedAppOp start(IBinder token,int op,int requestUid,String requestPackage,String attribution){
        int result=mode(op,requestUid,requestPackage);
        if(result==AppOpsManager.MODE_ALLOWED)synchronized(HostAppOps.class){
            if(!own(requestUid,requestPackage))return new SyncNotedAppOp(AppOpsManager.MODE_ERRORED,op,attribution,requestPackage);
            Active key=new Active(Objects.requireNonNull(token),op,requestUid,requestPackage,attribution);Started started=active.get(key);
            if(active.size()>=256&&started==null||started!=null&&started.count>=65536)return new SyncNotedAppOp(AppOpsManager.MODE_IGNORED,op,attribution,requestPackage);
            if(started==null){
                started=new Started(key);
                try{token.linkToDeath(started,0);}catch(RemoteException dead){throw new RuntimeException(dead);}
                if(started.retired)throw new RuntimeException(new DeadObjectException());
                active.put(key,started);
            }
            started.count++;
        }
        return new SyncNotedAppOp(result,op,attribution,requestPackage);
    }
    private static synchronized void finish(IBinder token,int op,int requestUid,String requestPackage,String attribution){
        Active key=new Active(token,op,requestUid,requestPackage,attribution);Started started=active.get(key);
        if(started!=null&&--started.count==0){active.remove(key);started.unlink();}
    }
    private static synchronized void watch(int op,String requestPackage,IAppOpsCallback callback){if(closed||callback==null)return;if(requestPackage!=null&&!requestPackage.equals(packageName))return;if(watchers.getRegisteredCallbackCount()>=128)throw new IllegalStateException("Application operation callback limit reached");watchers.register(callback,new Watch(op,requestPackage));}
    public static synchronized void permissionsChanged(){if(!installed||closed||pending)return;pending=true;if(!handler.post(HostAppOps::dispatchChanges))pending=false;}
    private static void dispatchChanges(){synchronized(HostAppOps.class){pending=false;if(closed)return;active.entrySet().removeIf(entry->{Active key=entry.getKey();if(mode(key.op,key.uid,key.packageName)==AppOpsManager.MODE_ALLOWED)return false;entry.getValue().unlink();return true;});}HostPermissions.notifyChanged();
        int count=watchers.beginBroadcast();try{for(int index=0;index<count;index++){Watch watch=(Watch)watchers.getBroadcastCookie(index);try{for(int op:new int[]{AppOpsManager.OP_COARSE_LOCATION,AppOpsManager.OP_FINE_LOCATION,AppOpsManager.OP_CAMERA,AppOpsManager.OP_RECORD_AUDIO,AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE})if(watch.op==-1||AppOpsManager.opToSwitch(watch.op)==AppOpsManager.opToSwitch(op))watchers.getBroadcastItem(index).opChanged(op,uid,packageName,"default:0");}catch(RemoteException error){unavailable("callback delivery");}}}finally{watchers.finishBroadcast();}}
    private static synchronized void close(){closed=true;for(var started:active.values())started.unlink();active.clear();globalRestrictions.clear();watchers.kill();if(handler!=null)handler.removeCallbacksAndMessages(null);HostServices.unregister(Context.APP_OPS_SERVICE,service,"com.android.internal.app.IAppOpsService");}
    private static Object invoke(Object proxy,Method method,Object[] arguments)throws Throwable{
        Object[] a=arguments==null?new Object[0]:arguments;String name=method.getName();
        if(method.getDeclaringClass()==Object.class)return switch(name){case "toString"->"Local Android application operations";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==a[0];default->throw new AssertionError(name);};
        switch(name){
            case "asBinder":return binder;
            case "checkPackage":return HostPackages.service().getPackageUid((String)a[1],0,UserHandle.getUserId((int)a[0]))==(int)a[0]?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_ERRORED;
            case "checkOperation":case "checkOperationRaw":return mode((int)a[0],(int)a[1],(String)a[2]);
            case "checkOperationRawForDevice":case "checkOperationForDevice":return (int)a[4]==0?mode((int)a[0],(int)a[1],(String)a[2]):AppOpsManager.MODE_IGNORED;
            case "checkAudioOperation":return mode((int)a[0],(int)a[2],(String)a[3]);
            case "permissionToOpCode":return AppOpsManager.permissionToOpCode((String)a[0]);
            case "noteOperation":return note((int)a[0],(int)a[1],(String)a[2],(String)a[3]);
            case "noteOperationForDevice":return (int)a[4]==0?note((int)a[0],(int)a[1],(String)a[2],(String)a[3]):new SyncNotedAppOp(AppOpsManager.MODE_IGNORED,(int)a[0],(String)a[3],(String)a[2]);
            case "startOperation":return start((IBinder)a[0],(int)a[1],(int)a[2],(String)a[3],(String)a[4]);
            case "startOperationForDevice":return (int)a[5]==0?start((IBinder)a[0],(int)a[1],(int)a[2],(String)a[3],(String)a[4]):new SyncNotedAppOp(AppOpsManager.MODE_IGNORED,(int)a[1],(String)a[4],(String)a[3]);
            case "finishOperation":case "finishOperationForDevice":finish((IBinder)a[0],(int)a[1],(int)a[2],(String)a[3],(String)a[4]);return null;
            case "noteProxyOperation":case "noteProxyOperationWithState":{AttributionSource source=a[1] instanceof AttributionSource s?s:new AttributionSource((AttributionSourceState)a[1]);return note((int)a[0],source.getUid(),source.getPackageName(),source.getAttributionTag());}
            case "startProxyOperation":case "startProxyOperationWithState":{AttributionSource source=a[2] instanceof AttributionSource s?s:new AttributionSource((AttributionSourceState)a[2]);return start((IBinder)a[0],(int)a[1],source.getUid(),source.getPackageName(),source.getAttributionTag());}
            case "finishProxyOperation":case "finishProxyOperationWithState":{AttributionSource source=a[2] instanceof AttributionSource s?s:new AttributionSource((AttributionSourceState)a[2]);finish((IBinder)a[0],(int)a[1],source.getUid(),source.getPackageName(),source.getAttributionTag());return null;}
            case "startWatchingMode":watch((int)a[0],(String)a[1],(IAppOpsCallback)a[2]);return null;
            case "startWatchingModeWithFlags":watch((int)a[0],(String)a[1],(IAppOpsCallback)a[3]);return null;
            case "stopWatchingMode":watchers.unregister((IAppOpsCallback)a[0]);return null;
            case "isOperationActive":synchronized(HostAppOps.class){int op=(int)a[0],requestUid=(int)a[1];String requestPackage=(String)a[2];return active.keySet().stream().anyMatch(key->key.op==op&&key.uid==requestUid&&Objects.equals(key.packageName,requestPackage));}
            case "shouldCollectNotes":return false;
            case "getHistoricalOps":case "getHistoricalOpsFromDiskRaw":unavailable(name);((RemoteCallback)a[a.length-1]).sendResult(Bundle.EMPTY);return null;
            default:unavailable(name);Class<?> result=method.getReturnType();if(result==void.class)return null;if(result==boolean.class)return false;if(result==int.class)return AppOpsManager.MODE_ERRORED;if(List.class.isAssignableFrom(result))return List.of();return null;
        }
    }
}
