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
        final Active key;int count;boolean retired,paused;
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
    private static final Set<Integer> changedOps=new LinkedHashSet<>();
    private static SharedPreferences decisions;
    private static boolean foregroundModes;
    private static Handler handler;private static String packageName;private static int uid;private static boolean installed,closed,pending;
    private static final SharedPreferences.OnSharedPreferenceChangeListener decisionListener=(preferences,key)->{
        if(key==null){permissionsChanged();return;}
        String prefix="appop:uid:"+uid+":";
        if(!key.startsWith("appop:")||key.startsWith("appop:uid:")&&!key.startsWith(prefix))return;
        String name=key.substring(key.startsWith(prefix)?prefix.length():"appop:".length());
        int op;try{op=AppOpsManager.strOpToOp(name);}catch(IllegalArgumentException unknown){return;}
        synchronized(HostAppOps.class){if(closed||!installed)return;refreshForegroundModes();changedOps.add(AppOpsManager.opToSwitch(op));postChanges();}
    };
    static {binder.attachInterface(service,"com.android.internal.app.IAppOpsService");}
    public static synchronized void install(Context context){
        if(closed)throw new IllegalStateException("Application operation service is closed");
        if(installed)return;packageName=context.getPackageName();uid=android.os.Process.myUid();handler=new Handler(Objects.requireNonNull(Looper.getMainLooper()));
        decisions=HostPermissions.decisions(context);decisions.registerOnSharedPreferenceChangeListener(decisionListener);
        refreshForegroundModes();
        HostServices.register(Context.APP_OPS_SERVICE,service,"com.android.internal.app.IAppOpsService");
        if(!HostLooper.addShutdownListener(handler.getLooper(),HostAppOps::close))throw new IllegalStateException("Application operation owner has stopped");installed=true;
    }
    public static AppOpsManager create(Context context){install(context);try{Constructor<AppOpsManager> constructor=AppOpsManager.class.getDeclaredConstructor(Context.class,IAppOpsService.class);constructor.setAccessible(true);return constructor.newInstance(context,service);}catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}}
    private static synchronized boolean own(int requestUid,String requestPackage){return !closed&&installed&&requestUid==uid&&Objects.equals(packageName,requestPackage);}
    private static void unavailable(String operation){synchronized(diagnostics){if(diagnostics.size()>=32||!diagnostics.add(operation))return;}System.err.println("[android-host] Application operation unavailable: "+operation);}
    private static int mode(int op,int requestUid,String requestPackage){return mode(op,requestUid,requestPackage,false);}
    private static int mode(int op,int requestUid,String requestPackage,boolean raw){
        if(!own(requestUid,requestPackage))return AppOpsManager.MODE_ERRORED;
        synchronized(HostAppOps.class){if(globalRestrictions.values().stream().anyMatch(ops->ops.contains(op)))return AppOpsManager.MODE_IGNORED;}
        int code;try{code=AppOpsManager.opToSwitch(op);}catch(IndexOutOfBoundsException|IllegalArgumentException error){return AppOpsManager.MODE_ERRORED;}
        int defaultMode=AppOpsManager.opToDefaultMode(code);
        int selected=decisions.getInt(uidKey(code),defaultMode);
        if(selected==defaultMode)selected=decisions.getInt(packageKey(code),defaultMode);
        if(selected!=AppOpsManager.MODE_ALLOWED){
            if(selected!=AppOpsManager.MODE_FOREGROUND||raw)return selected;
            if(HostActivityManager.hostUidProcessState(null,requestUid)>ActivityManager.PROCESS_STATE_FOREGROUND_SERVICE)return AppOpsManager.MODE_IGNORED;
        }
        if(op==AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE)return HostPermissions.requested(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE)?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_DEFAULT;
        String permission;try{permission=AppOpsManager.opToPermission(op);}catch(IndexOutOfBoundsException|IllegalArgumentException error){return AppOpsManager.MODE_ERRORED;}
        if(ManualLocation.isLocationPermission(permission))return ManualLocation.isGranted()?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_IGNORED;
        if(MediaInputs.isMediaPermission(permission))return MediaInputs.isGranted(permission)?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_IGNORED;
        if(permission!=null)return HostPermissions.check(permission,requestUid,Context.DEVICE_ID_DEFAULT)==android.content.pm.PackageManager.PERMISSION_GRANTED?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_IGNORED;
        return AppOpsManager.MODE_ALLOWED;
    }
    private static String uidKey(int op){return "appop:uid:"+uid+":"+AppOpsManager.opToPublicName(op);}
    // The existing all-files settings choice uses this same package key.
    private static String packageKey(int op){return "appop:"+AppOpsManager.opToPublicName(op);}
    private static void refreshForegroundModes(){foregroundModes=decisions.getAll().entrySet().stream().anyMatch(entry->entry.getKey().startsWith("appop:")&&Objects.equals(entry.getValue(),AppOpsManager.MODE_FOREGROUND));}
    private static void setMode(int op,int requestUid,String requestPackage,int value,boolean uidMode){
        int caller=Binder.getCallingUid();var peer=HostBinderRpc.callingPeer();
        if(peer!=null&&(peer.uid()!=caller||peer.pid()!=Binder.getCallingPid()))throw new SecurityException("Application-operation caller differs from its authenticated process");
        if(HostPermissions.check(android.Manifest.permission.MANAGE_APP_OPS_MODES,caller,Context.DEVICE_ID_DEFAULT)!=android.content.pm.PackageManager.PERMISSION_GRANTED)
            throw new SecurityException("Application-operation mode changes require "+android.Manifest.permission.MANAGE_APP_OPS_MODES);
        if(op<0||op>=AppOpsManager._NUM_OP)throw new IllegalArgumentException("Invalid application operation: "+op);
        if(value<AppOpsManager.MODE_ALLOWED||value>AppOpsManager.MODE_FOREGROUND)throw new IllegalArgumentException("Invalid application-operation mode: "+value);
        if(requestUid<0)throw new IllegalArgumentException("Invalid application-operation UID: "+requestUid);
        int code=AppOpsManager.opToSwitch(op);String key;
        synchronized(HostAppOps.class){
            if(closed||!installed)throw new IllegalStateException("Application-operation owner is closed");
            if(requestUid!=uid)throw new SecurityException("Application-operation UID belongs to another process owner");
            if(!uidMode&&!Objects.equals(requestPackage,packageName))return;
            key=uidMode?uidKey(code):packageKey(code);
            int defaultMode=AppOpsManager.opToDefaultMode(code);
            if(decisions.getInt(key,defaultMode)==value)return;
            var edit=decisions.edit();if(value==defaultMode)edit.remove(key);else edit.putInt(key,value);edit.apply();
            refreshForegroundModes();
            if(value!=AppOpsManager.MODE_ERRORED)for(var entry:active.entrySet()){
                Active activeOp=entry.getKey();
                if(activeOp.op==code)entry.getValue().paused=mode(activeOp.op,activeOp.uid,activeOp.packageName)!=AppOpsManager.MODE_ALLOWED;
            }
            changedOps.add(code);postChanges();
        }
        AppOpsManager.invalidateAppOpModeCache();
    }
    static void uidStateChanged(){synchronized(HostAppOps.class){if(!installed||closed||!foregroundModes)return;}permissionsChanged();}
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
    private static synchronized void watch(int op,String requestPackage,IAppOpsCallback callback){if(closed||callback==null)return;if(op< -1||op>=AppOpsManager._NUM_OP)throw new IllegalArgumentException("Invalid watched application operation: "+op);if(requestPackage!=null&&!requestPackage.equals(packageName))return;if(watchers.getRegisteredCallbackCount()>=128)throw new IllegalStateException("Application operation callback limit reached");watchers.register(callback,new Watch(op,requestPackage));}
    private static void postChanges(){if(pending)return;pending=true;if(!handler.post(HostAppOps::dispatchChanges))pending=false;}
    public static synchronized void permissionsChanged(){if(!installed||closed)return;for(int op=0;op<AppOpsManager._NUM_OP;op++)changedOps.add(AppOpsManager.opToSwitch(op));postChanges();}
    private static void dispatchChanges(){Set<Integer> changed;synchronized(HostAppOps.class){pending=false;if(closed)return;changed=Set.copyOf(changedOps);changedOps.clear();for(var entry:active.entrySet()){Active key=entry.getKey();int selected=mode(key.op,key.uid,key.packageName);if(selected!=AppOpsManager.MODE_ERRORED)entry.getValue().paused=selected!=AppOpsManager.MODE_ALLOWED;}}HostPermissions.notifyChanged();
        int count=watchers.beginBroadcast();try{for(int index=0;index<count;index++){Watch watch=(Watch)watchers.getBroadcastCookie(index);try{for(int op:changed)if(watch.op==-1||AppOpsManager.opToSwitch(watch.op)==op)watchers.getBroadcastItem(index).opChanged(op,uid,packageName,"default:0");}catch(RemoteException error){unavailable("callback delivery");}}}finally{watchers.finishBroadcast();}}
    private static synchronized void close(){closed=true;for(var started:active.values())started.unlink();active.clear();globalRestrictions.clear();changedOps.clear();watchers.kill();if(decisions!=null)decisions.unregisterOnSharedPreferenceChangeListener(decisionListener);if(handler!=null)handler.removeCallbacksAndMessages(null);HostServices.unregister(Context.APP_OPS_SERVICE,service,"com.android.internal.app.IAppOpsService");}
    private static Object invoke(Object proxy,Method method,Object[] arguments)throws Throwable{
        Object[] a=arguments==null?new Object[0]:arguments;String name=method.getName();
        if(method.getDeclaringClass()==Object.class)return switch(name){case "toString"->"Local Android application operations";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==a[0];default->throw new AssertionError(name);};
        switch(name){
            case "asBinder":return binder;
            case "checkPackage":return HostPackages.service().getPackageUid((String)a[1],0,UserHandle.getUserId((int)a[0]))==(int)a[0]?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_ERRORED;
            case "checkOperation":return mode((int)a[0],(int)a[1],(String)a[2]);
            case "checkOperationRaw":return mode((int)a[0],(int)a[1],(String)a[2],true);
            case "checkOperationRawForDevice":case "checkOperationForDevice":return (int)a[4]==0?mode((int)a[0],(int)a[1],(String)a[2],name.equals("checkOperationRawForDevice")):AppOpsManager.MODE_IGNORED;
            case "setUidMode":setMode((int)a[0],(int)a[1],null,(int)a[2],true);return null;
            case "setMode":setMode((int)a[0],(int)a[1],(String)a[2],(int)a[3],false);return null;
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
            case "isOperationActive":synchronized(HostAppOps.class){int op=(int)a[0],requestUid=(int)a[1];String requestPackage=(String)a[2];return active.entrySet().stream().anyMatch(entry->{Active key=entry.getKey();return !entry.getValue().paused&&key.op==op&&key.uid==requestUid&&Objects.equals(key.packageName,requestPackage);});}
            case "shouldCollectNotes":return false;
            case "getHistoricalOps":case "getHistoricalOpsFromDiskRaw":unavailable(name);((RemoteCallback)a[a.length-1]).sendResult(Bundle.EMPTY);return null;
            default:unavailable(name);Class<?> result=method.getReturnType();if(result==void.class)return null;if(result==boolean.class)return false;if(result==int.class)return AppOpsManager.MODE_ERRORED;if(List.class.isAssignableFrom(result))return List.of();return null;
        }
    }
}
