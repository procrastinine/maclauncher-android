package maclauncher.android.framework;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.*;
import android.app.Activity;
import android.app.ActivityManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.permission.IPermissionManager;
import android.permission.PermissionManager;
import android.os.Process;
import java.nio.file.Path;
import java.util.*;

/** Original manifest protection levels plus explicit sensitive-device grants. */
public final class HostPermissions extends IPermissionManager.Default {
    private static final HostPermissions INSTANCE=new HostPermissions();
    private static Map<String,java.util.function.IntFunction<PermissionInfo>> declarations=Map.of();
    private static Map<String,java.util.function.IntFunction<PermissionGroupInfo>> groups=Map.of();
    private static Set<String> requested=Set.of(),normal=Set.of();
    private static boolean installed;
    private static android.permission.IPermissionChecker checker;
    private static final String DECISIONS="maclauncher.platform.permission-decisions";
    private static final android.os.RemoteCallbackList<android.permission.IOnPermissionsChangeListener> listeners=new android.os.RemoteCallbackList<>();
    private static volatile RemotePermissions remotePermissions;
    /** Connection leases only: grant state remains in each application's original permission service. */
    private static final class RemotePermissions implements AutoCloseable {
        private final Map<HostProcessServices.Process,Lease> leases=new LinkedHashMap<>();
        private AutoCloseable observation;
        private boolean closed;
        private final class Lease implements IBinder.DeathRecipient {
            final HostProcessServices.Directory directory;
            final IPermissionManager service;
            final android.permission.IOnPermissionsChangeListener listener=new android.permission.IOnPermissionsChangeListener.Stub(){
                @Override public void onPermissionsChanged(int uid,String device){if(uid==directory.peer().uid())notifyChanged(uid,device);}
            };
            boolean registered;
            Lease(HostProcessServices.Directory directory,IPermissionManager service){this.directory=directory;this.service=service;}
            @Override public void binderDied(){retire(this);}
        }
        void changed(HostProcessServices.Directory directory){
            if(!directory.available()){Lease lease;synchronized(this){lease=leases.get(directory.process());}if(lease!=null)retire(lease);return;}
            // Never await process startup: this notification is issued only after root exchange.
            try{
                IBinder binder=directory.process().service("permissionmgr");if(binder==null)return;
                Lease lease=new Lease(directory,IPermissionManager.Stub.asInterface(binder));
                binder.linkToDeath(lease,0);
                synchronized(this){if(closed||leases.containsKey(directory.process())){binder.unlinkToDeath(lease,0);return;}leases.put(directory.process(),lease);}
                try{
                    lease.service.addOnPermissionsChangeListener(lease.listener);
                    boolean active;synchronized(this){active=leases.get(directory.process())==lease;if(active)lease.registered=true;}
                    if(!active){lease.service.removeOnPermissionsChangeListener(lease.listener);return;}
                    directory.process().identity();
                }
                catch(android.os.RemoteException|RuntimeException unavailable){retire(lease);return;}
                notifyChanged(directory.peer().uid(),"default:0");
            }catch(android.os.RemoteException|IllegalStateException unavailable){/* The authenticated process retired during publication. */}
        }
        int check(String permission,int uid,int device){
            List<Lease> snapshot;synchronized(this){snapshot=leases.values().stream().filter(value->value.directory.peer().uid()==uid).toList();}
            for(Lease lease:snapshot)try{
                if(lease.service.checkUidPermission(uid,permission,device)==PackageManager.PERMISSION_GRANTED){
                    // A result from a retired/replaced transport is no longer authoritative.
                    synchronized(this){if(leases.get(lease.directory.process())==lease)return PackageManager.PERMISSION_GRANTED;}
                }
            }catch(android.os.RemoteException unavailable){retire(lease);}
            return PackageManager.PERMISSION_DENIED;
        }
        void retire(Lease lease){
            boolean registered;synchronized(this){if(!leases.remove(lease.directory.process(),lease))return;registered=lease.registered;}
            try{lease.service.asBinder().unlinkToDeath(lease,0);}catch(NoSuchElementException gone){}
            if(registered)try{lease.service.removeOnPermissionsChangeListener(lease.listener);}catch(android.os.RemoteException gone){}
            notifyChanged(lease.directory.peer().uid(),"default:0");
        }
        @Override public void close(){
            List<Lease> snapshot;synchronized(this){if(closed)return;closed=true;snapshot=List.copyOf(leases.values());}
            if(observation!=null)try{observation.close();}catch(Exception failure){throw new IllegalStateException("Android permission observation could not close",failure);}
            for(Lease lease:snapshot)retire(lease);
            synchronized(HostPermissions.class){if(remotePermissions==this)remotePermissions=null;}
        }
    }
    /** The canonical platform owner observes enrolled directories, never creates another grant store. */
    public static AutoCloseable remote(HostProcessServices processes){
        var owner=new RemotePermissions();synchronized(HostPermissions.class){if(remotePermissions!=null)throw new IllegalStateException("Android permission directories are already owned");remotePermissions=owner;}
        try{owner.observation=processes.observeDirectories(owner::changed);return owner;}catch(RuntimeException failure){owner.close();throw failure;}
    }
    static int checkRemote(String permission,int uid,int device){var owner=remotePermissions;return owner==null||uid==Process.myUid()?PackageManager.PERMISSION_DENIED:owner.check(permission,uid,device);}
    // Original SystemConfig is a process singleton. Retain that owner instead
    // of re-entering its system-server caller diagnostic for every app check.
    private static final class PlatformConfiguration {
        static final com.android.server.SystemConfig INSTANCE=com.android.server.SystemConfig.getInstance();
    }
    static com.android.server.SystemConfig platformConfiguration(){return PlatformConfiguration.INSTANCE;}
    private HostPermissions(){}
    static synchronized void configure(Path framework,com.android.server.pm.pkg.AndroidPackage application)throws Exception{
        var permissions=new LinkedHashMap<String,java.util.function.IntFunction<PermissionInfo>>();
        var permissionGroups=new LinkedHashMap<String,java.util.function.IntFunction<PermissionGroupInfo>>();
        {
            var platform=HostPackageParser.parse(framework,PackageParser.PARSE_IS_SYSTEM_DIR);
            for(var permission:platform.getPermissions())permissions.put(permission.getName(),flags->com.android.server.pm.parsing.PackageInfoUtils.generatePermissionInfo(permission,flags));
            for(var group:platform.getPermissionGroups())permissionGroups.put(group.getName(),flags->com.android.server.pm.parsing.PackageInfoUtils.generatePermissionGroupInfo(group,flags));
        }
        for(var permission:application.getPermissions())permissions.putIfAbsent(permission.getName(),flags->com.android.server.pm.parsing.PackageInfoUtils.generatePermissionInfo(permission,flags));
        for(var group:application.getPermissionGroups())permissionGroups.putIfAbsent(group.getName(),flags->com.android.server.pm.parsing.PackageInfoUtils.generatePermissionGroupInfo(group,flags));
        Set<String> granted=new HashSet<>();for(String name:application.getRequestedPermissions()){var definition=permissions.get(name);if(definition!=null&&(definition.apply(0).protectionLevel&PermissionInfo.PROTECTION_MASK_BASE)==PermissionInfo.PROTECTION_NORMAL)granted.add(name);}
        declarations=Map.copyOf(permissions);groups=Map.copyOf(permissionGroups);requested=Set.copyOf(application.getRequestedPermissions());normal=Set.copyOf(granted);
    }
    static synchronized void clear(){declarations=Map.of();groups=Map.of();requested=Set.of();normal=Set.of();listeners.kill();}
    static synchronized void install(){if(installed)return;HostServices.register("permissionmgr",INSTANCE,"android.permission.IPermissionManager");PermissionManager.disablePermissionCache();PermissionManager.disablePackageNamePermissionCache();installed=true;}
    static synchronized void installChecker(Context context){
        if(checker!=null)return;
        checker=(android.permission.IPermissionChecker)HostUiAccess.construct(HostUiAccess.type("com.android.server.pm.permission.PermissionManagerService$PermissionCheckerService"),new Class<?>[]{Context.class},context);
        HostServices.register("permission_checker",checker,"android.permission.IPermissionChecker");
    }
    static int check(String permission,int uid,int device){
        // Android's system-server and root identities are platform principals,
        // assigned only through the authenticated supervisor, never APK metadata.
        if(uid==Process.SYSTEM_UID||uid==Process.ROOT_UID)return PackageManager.PERMISSION_GRANTED;
        // Native Android services receive the exact grants declared for their
        // UID by the original platform configuration, not an application's
        // manifest or a blanket privileged-service exemption.
        var system=PlatformConfiguration.INSTANCE.getSystemPermissions().get(uid);
        if(system!=null&&system.contains(permission))return PackageManager.PERMISSION_GRANTED;
        int local=checkLocal(permission,uid,device);return local==PackageManager.PERMISSION_GRANTED?local:HostPackages.checkUidPermission(permission,uid,device);
    }
    static synchronized int checkLocal(String permission,int uid,int device){
        Objects.requireNonNull(permission);if(uid!=Process.myUid()||device!=Context.DEVICE_ID_DEFAULT)return PackageManager.PERMISSION_DENIED;
        boolean granted=normal.contains(permission)||requested.contains(permission)&&(
            maclauncher.android.location.ManualLocation.isLocationPermission(permission)&&maclauncher.android.location.ManualLocation.isGranted()
            ||maclauncher.android.media.MediaInputs.isMediaPermission(permission)&&maclauncher.android.media.MediaInputs.isGranted(permission));
        return granted?PackageManager.PERMISSION_GRANTED:PackageManager.PERMISSION_DENIED;
    }
    static Set<String> grantedPermissions(){
        Set<String> snapshot;synchronized(HostPermissions.class){snapshot=requested;}
        // Signature grants can verify original APK contents or query a remote
        // permission owner. Neither operation may hold the local grant lock.
        Set<String> result=new HashSet<>();for(String permission:snapshot)if(check(permission,Process.myUid(),Context.DEVICE_ID_DEFAULT)==PackageManager.PERMISSION_GRANTED)result.add(permission);return result;
    }
    static synchronized Set<String> normalPermissions(com.android.server.pm.pkg.AndroidPackage pkg){Set<String> result=new HashSet<>();for(String permission:pkg.getRequestedPermissions()){var declaration=declarations.get(permission);if(declaration!=null&&(declaration.apply(0).protectionLevel&PermissionInfo.PROTECTION_MASK_BASE)==PermissionInfo.PROTECTION_NORMAL)result.add(permission);}return result;}
    private static boolean owns(String pkg,int user){return Objects.equals(pkg,HostContext.hostApplicationInfoSnapshot().packageName)&&user==Process.myUserHandle().getIdentifier();}
    private static synchronized boolean runtimePermission(String permission){var declaration=declarations.get(permission);return requested.contains(permission)&&declaration!=null&&(declaration.apply(0).protectionLevel&PermissionInfo.PROTECTION_MASK_BASE)==PermissionInfo.PROTECTION_DANGEROUS;}
    // Use Android's original preference implementation and its existing disk
    // owner. Decisions are private to this installation, just like its grants.
    static SharedPreferences decisions(){return decisions(HostApplication.applicationContext());}
    static SharedPreferences decisions(Context context){return context.createDeviceProtectedStorageContext().getSharedPreferences(DECISIONS,Context.MODE_PRIVATE);}
    static synchronized boolean requested(String permission){return requested.contains(permission);}
    private static int decisionFlags(String permission){return decisions().getInt(permission,0);}
    static void recordResult(String[] names,int[] results){
        SharedPreferences.Editor changed=null;boolean observed=false;
        for(int i=0;i<names.length;i++)if(runtimePermission(names[i])){
            observed=true;
            int before=decisionFlags(names[i]),after=before|PackageManager.FLAG_PERMISSION_USER_SET;
            if(results[i]==PackageManager.PERMISSION_GRANTED)after&=~PackageManager.FLAG_PERMISSION_USER_FIXED;
            if(before!=after){if(changed==null)changed=decisions().edit();changed.putInt(names[i],after);}
        }
        if(changed!=null)changed.apply();
        // Physical authorization can change without changing Android's retained decision flags.
        if(observed)notifyChanged();
    }
    /** The host handles the platform permission destination, without inventing an installed APK. */
    static boolean handles(Intent intent){return PackageManager.ACTION_REQUEST_PERMISSIONS.equals(intent.getAction())&&intent.getPackage()==null&&intent.getComponent()==null;}
    static int request(Intent intent,IBinder token,String who,int requestCode){
        if(token==null)return ActivityManager.START_PERMISSION_DENIED;
        String[] permissions=intent.getStringArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES);
        if(permissions==null)return ActivityManager.START_INTENT_NOT_RESOLVED;
        String[] names=permissions.clone();for(String name:names)Objects.requireNonNull(name);
        Handler main=new Handler(Looper.getMainLooper());
        // ActivityTaskManager callers can be render or Binder threads. The
        // application record and result dispatch belong to ActivityThread's Looper.
        if(!main.post(()->{
            var process=HostApplication.current();
            Activity activity=process.thread.getActivity(token);if(activity==null||activity.isDestroyed())return;
            Runnable finish=()->{
                Activity current=process.thread.getActivity(token);if(current==null||current.isDestroyed())return;
                int[] results=new int[names.length];for(int i=0;i<names.length;i++)results[i]=check(names[i],Process.myUid(),Context.DEVICE_ID_DEFAULT);
                recordResult(names,results);
                Intent result=new Intent().putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES,names).putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS,results);
                process.thread.sendActivityResult(token,who,requestCode,Activity.RESULT_OK,result);
            };
            boolean location; synchronized(HostPermissions.class){location=Arrays.stream(names).anyMatch(name->requested.contains(name)&&maclauncher.android.location.ManualLocation.isLocationPermission(name));}
            if(location)maclauncher.android.location.ManualLocation.request(value->{if(Looper.myLooper()==main.getLooper())finish.run();else main.post(finish);});else finish.run();
        }))return ActivityManager.START_CANCELED;
        return ActivityManager.START_SUCCESS;
    }
    @Override public PermissionInfo getPermissionInfo(String name,String caller,int flags){var value=declarations.get(name);return value==null?null:value.apply(flags);}
    @Override public PermissionGroupInfo getPermissionGroupInfo(String name,int flags){var value=groups.get(name);return value==null?null:value.apply(flags);}
    @Override public ParceledListSlice<PermissionGroupInfo> getAllPermissionGroups(int flags){return new ParceledListSlice<>(groups.values().stream().map(value->value.apply(flags)).toList());}
    @Override public ParceledListSlice<PermissionInfo> queryPermissionsByGroup(String group,int flags){return new ParceledListSlice<>(declarations.values().stream().filter(value->Objects.equals(group,value.apply(0).group)).map(value->value.apply(flags)).toList());}
    @Override public int checkUidPermission(int uid,String permission,int device){return check(permission,uid,device);}
    @Override public int getPermissionFlags(String pkg,String permission,String device,int user){return owns(pkg,user)&&runtimePermission(permission)?decisionFlags(permission):0;}
    @Override public boolean shouldShowRequestPermissionRationale(String pkg,String permission,int device,int user){
        if(!owns(pkg,user)||device!=Context.DEVICE_ID_DEFAULT||!runtimePermission(permission)||check(permission,Process.myUid(),device)==PackageManager.PERMISSION_GRANTED)return false;
        int flags=decisionFlags(permission);
        int fixed=PackageManager.FLAG_PERMISSION_USER_FIXED|PackageManager.FLAG_PERMISSION_POLICY_FIXED|PackageManager.FLAG_PERMISSION_SYSTEM_FIXED;
        return (flags&fixed)==0&&(flags&PackageManager.FLAG_PERMISSION_USER_SET)!=0;
    }
    @Override public int getPermissionRequestState(String pkg,String permission,int device){
        if(!owns(pkg,Process.myUserHandle().getIdentifier())||device!=Context.DEVICE_ID_DEFAULT)return Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE;
        if(check(permission,Process.myUid(),device)==PackageManager.PERMISSION_GRANTED)return Context.PERMISSION_REQUEST_STATE_GRANTED;
        int fixed=PackageManager.FLAG_PERMISSION_USER_FIXED|PackageManager.FLAG_PERMISSION_POLICY_FIXED|PackageManager.FLAG_PERMISSION_SYSTEM_FIXED;
        return runtimePermission(permission)&&(decisionFlags(permission)&fixed)==0?Context.PERMISSION_REQUEST_STATE_REQUESTABLE:Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE;
    }
    @Override public Map<String,PermissionManager.PermissionState> getAllPermissionStates(String pkg,String device,int user){
        if(!owns(pkg,user)||!Objects.equals(device,"default:0"))return Map.of();
        var result=new LinkedHashMap<String,PermissionManager.PermissionState>();
        for(String permission:requested)result.put(permission,new PermissionManager.PermissionState(check(permission,Process.myUid(),Context.DEVICE_ID_DEFAULT)==PackageManager.PERMISSION_GRANTED,runtimePermission(permission)?decisionFlags(permission):0));
        return result;
    }
    @Override public void addOnPermissionsChangeListener(android.permission.IOnPermissionsChangeListener listener){listeners.register(Objects.requireNonNull(listener));}
    @Override public void removeOnPermissionsChangeListener(android.permission.IOnPermissionsChangeListener listener){listeners.unregister(Objects.requireNonNull(listener));}
    static void notifyChanged(){notifyChanged(Process.myUid(),"default:0");}
    private static void notifyChanged(int uid,String device){
        android.app.PropertyInvalidatedCache.invalidateCache(PermissionManager.CACHE_KEY_PACKAGE_INFO_CACHE);
        List<android.permission.IOnPermissionsChangeListener> snapshot=new ArrayList<>();
        synchronized(listeners){int count=listeners.beginBroadcast();try{for(int i=0;i<count;i++)snapshot.add(listeners.getBroadcastItem(i));}finally{listeners.finishBroadcast();}}
        // Reverse Binder calls never hold the permission or callback-list owner lock.
        for(var listener:snapshot)try{listener.onPermissionsChanged(uid,device);}catch(android.os.RemoteException gone){}
    }
    @Override public java.util.List<android.content.pm.permission.SplitPermissionInfoParcelable> getSplitPermissions(){
        return PlatformConfiguration.INSTANCE.getSplitPermissions().stream().map(value->new android.content.pm.permission.SplitPermissionInfoParcelable(value.getSplitPermission(),value.getNewPermissions(),value.getTargetSdk())).toList();
    }
    @Override public int checkPermission(String pkg,String permission,String device,int user){return Objects.equals(pkg,HostContext.hostApplicationInfoSnapshot().packageName)&&user==Process.myUserHandle().getIdentifier()?check(permission,Process.myUid(),Context.DEVICE_ID_DEFAULT):PackageManager.PERMISSION_DENIED;}
}
