package maclauncher.android.framework;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityManager.MemoryInfo;
import android.app.ActivityManager.RunningAppProcessInfo;
import android.app.ApplicationExitInfo;

import android.content.Context;
import android.content.ComponentName;
import android.os.Parcel;
import android.os.Parcelable;
import maclauncher.android.bridge.Bridge;
import java.util.*;

/** Process-local Android queries. Memory uses Klepton's existing guest budget. */
public final class HostActivityManager {
    private HostActivityManager(){}
    private static final Object graphicsLock=new Object();
    private static volatile int graphicsVersion;
    private static boolean graphicsQueried;
    /** Query before Application code can create a guest EGL context. */
    public static void hostInitializeGraphics(){
        synchronized(graphicsLock){
            if(graphicsQueried)return;graphicsQueried=true;
            try{graphicsVersion=queryGraphics((javax.microedition.khronos.egl.EGL10)javax.microedition.khronos.egl.EGLContext.getEGL());}
            catch(RuntimeException|LinkageError unavailable){System.err.println("[android-host] Graphics capability unavailable: "+unavailable);}
        }
    }
    public static int hostGraphicsVersion(){hostInitializeGraphics();return graphicsVersion;}
    public static List<android.content.pm.FeatureInfo> hostHardwareFeatures(){
        var features=namedHardwareFeatures(null);
        var graphics=new android.content.pm.FeatureInfo();graphics.reqGlEsVersion=hostGraphicsVersion();features.add(graphics);
        return features;
    }
    private static List<android.content.pm.FeatureInfo> namedHardwareFeatures(String requested){
        var features=new ArrayList<android.content.pm.FeatureInfo>();
        // These describe existing virtual input, display and playback owners.
        // Host capture devices and physical hardware remain separate capabilities.
        for(String name:List.of(android.content.pm.PackageManager.FEATURE_TOUCHSCREEN,
            android.content.pm.PackageManager.FEATURE_FAKETOUCH,
            android.content.pm.PackageManager.FEATURE_AUDIO_OUTPUT,
            android.content.pm.PackageManager.FEATURE_SCREEN_LANDSCAPE,
            android.content.pm.PackageManager.FEATURE_SCREEN_PORTRAIT,
            ActivityManager.isLowRamDeviceStatic()?android.content.pm.PackageManager.FEATURE_RAM_LOW:android.content.pm.PackageManager.FEATURE_RAM_NORMAL)){
            if(requested==null||name.equals(requested)){
                var feature=new android.content.pm.FeatureInfo();feature.name=name;features.add(feature);
            }
        }
        if((requested==null||android.content.pm.PackageManager.FEATURE_WEBVIEW.equals(requested))&&maclauncher.android.web.WebKitFactory.available()){
            var web=new android.content.pm.FeatureInfo();web.name=android.content.pm.PackageManager.FEATURE_WEBVIEW;features.add(web);
        }
        if(requested==null||android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_VERSION.equals(requested)||android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL.equals(requested)){
            int version=maclauncher.android.bridge.Graphics.vulkanVersion();
            int level=maclauncher.android.bridge.Graphics.vulkanHardwareLevel();
            if(version!=0&&level>=0){
                var vulkan=new android.content.pm.FeatureInfo();vulkan.name=android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_VERSION;vulkan.version=version;features.add(vulkan);
                var hardware=new android.content.pm.FeatureInfo();hardware.name=android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL;hardware.version=level;features.add(hardware);
            }
        }
        return features;
    }
    public static boolean hostHasSystemFeature(String name,int version){
        if(name==null)return false;
        // The GLES capability row has no feature name. Named checks must not
        // initialize EGL or unrelated physical suppliers while answering them.
        for(var feature:namedHardwareFeatures(name))if(name.equals(feature.name))return feature.version>=version;
        return false;
    }
    public static android.content.pm.ConfigurationInfo getDeviceConfigurationInfo(Context context){
        var result=new android.content.pm.ConfigurationInfo();result.reqGlEsVersion=hostGraphicsVersion();
        result.reqKeyboardType=context.getResources().getConfiguration().keyboard;
        for(int id:android.view.InputDevice.getDeviceIds()){
            var device=android.view.InputDevice.getDevice(id);
            if(device!=null&&(device.getSources()&0x1002)==0x1002){result.reqTouchScreen=3;break;}
        }
        return result;
    }
    public static int queryGraphics(javax.microedition.khronos.egl.EGL10 egl){
        var current=egl.eglGetCurrentContext();
        if(current!=null&&!current.equals(javax.microedition.khronos.egl.EGL10.EGL_NO_CONTEXT))throw new IllegalStateException("Graphics capability query follows guest context creation");
        var display=egl.eglGetDisplay(javax.microedition.khronos.egl.EGL10.EGL_DEFAULT_DISPLAY);
        if(display==null||display.equals(javax.microedition.khronos.egl.EGL10.EGL_NO_DISPLAY)||!egl.eglInitialize(display,new int[2]))throw new IllegalStateException("ANGLE capability display unavailable");
        Throwable failure=null;
        try{
            var config=graphicsConfiguration(egl,display,0x40);
            if(config!=null)for(int minor=2;minor>=0;minor--){int version=graphicsContextVersion(egl,display,config,3,minor);if(version!=0)return version;}
            config=graphicsConfiguration(egl,display,4);
            return config==null?0:graphicsContextVersion(egl,display,config,2,0);
        }
        catch(RuntimeException|Error error){failure=error;throw error;}
        finally{
            try{if(!egl.eglTerminate(display))throw new IllegalStateException("ANGLE capability display cleanup failed");}
            catch(RuntimeException|Error cleanup){if(failure==null)throw cleanup;if(cleanup!=failure)failure.addSuppressed(cleanup);}
        }
    }
    private static javax.microedition.khronos.egl.EGLConfig graphicsConfiguration(javax.microedition.khronos.egl.EGL10 egl,javax.microedition.khronos.egl.EGLDisplay display,int bit){
        var configs=new javax.microedition.khronos.egl.EGLConfig[1];int[] count={0},flags={0};
        if(!egl.eglChooseConfig(display,new int[]{0x3033,1,0x3040,bit,0x3038},configs,1,count))throw new IllegalStateException("ANGLE configuration query failed");
        if(count[0]==0)return null;
        if(count[0]!=1||configs[0]==null||!egl.eglGetConfigAttrib(display,configs[0],0x3040,flags)||(flags[0]&bit)==0)
            throw new IllegalStateException("ANGLE capability configuration unavailable");
        return configs[0];
    }
    private static int graphicsContextVersion(javax.microedition.khronos.egl.EGL10 egl,javax.microedition.khronos.egl.EGLDisplay display,javax.microedition.khronos.egl.EGLConfig config,int major,int minor){
        var context=egl.eglCreateContext(display,config,javax.microedition.khronos.egl.EGL10.EGL_NO_CONTEXT,new int[]{0x3098,major,0x30fb,minor,0x3038});
        if(context==null||context.equals(javax.microedition.khronos.egl.EGL10.EGL_NO_CONTEXT)){egl.eglGetError();return 0;}
        try{
            int[] actualMajor={0},actualMinor={0};
            if(!egl.eglQueryContext(display,context,0x3098,actualMajor)||!egl.eglQueryContext(display,context,0x30fb,actualMinor))throw new IllegalStateException("ANGLE context version unavailable");
            return actualMajor[0]<<16|actualMinor[0];
        }finally{if(!egl.eglDestroyContext(display,context))throw new IllegalStateException("ANGLE capability context cleanup failed");}
    }
    private static volatile List<ApplicationExitInfo> exitHistory;
    private static ApplicationExitInfo exitRecord(int pid,int uid,String processName,long timestamp,int reason,int status){
        if(pid<=0||uid<0||processName==null||processName.isEmpty()||processName.length()>1024||processName.indexOf('\0')>=0||
            timestamp<0||reason<0||reason>16||status<0||status>255)throw new IllegalArgumentException("Invalid application exit record");
        var info=new ApplicationExitInfo();info.setPid(pid);info.setRealUid(uid);info.setPackageUid(uid);info.setDefiningUid(uid);
        info.setProcessName(processName);info.setTimestamp(timestamp);info.setReason(reason);info.setStatus(status);
        info.setDescription(reason==ApplicationExitInfo.REASON_SIGNALED?"Process terminated by signal "+status:reason==ApplicationExitInfo.REASON_EXIT_SELF?"Process exited with status "+status:null);
        return info;
    }
    /** One bounded snapshot from this application's launcher-owned records. */
    public static void hostExitHistory(String json)throws org.json.JSONException{
        if(json==null){exitHistory=null;return;}
        if(json.length()>8192)throw new IllegalArgumentException("Application exit history is too large");
        var array=new org.json.JSONArray(json);if(array.length()>32)throw new IllegalArgumentException("Too many application exit records");
        var records=new ArrayList<ApplicationExitInfo>();
        for(int i=0;i<array.length();i++){
            var value=array.getJSONObject(i);int reason=value.getInt("reason");
            if(reason!=ApplicationExitInfo.REASON_EXIT_SELF&&reason!=ApplicationExitInfo.REASON_SIGNALED)
                throw new IllegalArgumentException("Unobserved application exit reason");
            records.add(exitRecord(value.getInt("pid"),value.getInt("uid"),value.getString("processName"),
                value.getLong("timestamp"),reason,value.getInt("status")));
        }
        records.sort(Comparator.comparingLong(ApplicationExitInfo::getTimestamp).reversed());exitHistory=List.copyOf(records);
    }
    public static List<ApplicationExitInfo> getHistoricalProcessExitReasons(Context context,String packageName,int pid,int maxNum){
        if(pid<0||maxNum<0)throw new IllegalArgumentException("Negative process history filter");
        if(packageName!=null&&!context.getPackageName().equals(packageName))throw new SecurityException("Another application's process history is unavailable");
        var records=exitHistory;if(records==null)return List.of();
        var result=new ArrayList<ApplicationExitInfo>();
        for(var info:records)if(pid==0||pid==info.getPid()){result.add(new ApplicationExitInfo(info));if(maxNum!=0&&result.size()>=maxNum)break;}
        return result;
    }
    public static boolean isLowMemoryKillReportSupported(){return false;}
    public static void hostClearExitHistory(){exitHistory=null;}
    private static final Set<Activity> started=Collections.newSetFromMap(new WeakHashMap<>());
    private static java.util.function.BiConsumer<Integer,Integer> uidObserver;
    private static volatile HostProcessServices processDirectory;
    public static synchronized void hostProcessDirectory(HostProcessServices directory){
        Objects.requireNonNull(directory);if(processDirectory!=null&&processDirectory!=directory)throw new IllegalStateException("Process directory already has an owner");processDirectory=directory;
    }
    public static synchronized void hostRetireProcessDirectory(HostProcessServices directory){if(processDirectory==directory)processDirectory=null;}
    public static void hostStarted(Activity activity){
        java.util.function.BiConsumer<Integer,Integer> observer;int state;
        synchronized(HostActivityManager.class){boolean changed=started.isEmpty();if(!started.add(activity)||!changed)return;observer=uidObserver;state=ActivityManager.PROCESS_STATE_TOP;}
        if(observer!=null)observer.accept(android.os.Process.myUid(),state);
        maclauncher.android.ProcessAuthority.processStateChanged(state);
        HostAppOps.uidStateChanged();
    }
    public static void hostStopped(Activity activity){
        java.util.function.BiConsumer<Integer,Integer> observer;
        synchronized(HostActivityManager.class){if(!started.remove(activity)||!started.isEmpty())return;observer=uidObserver;}
        if(observer!=null)observer.accept(android.os.Process.myUid(),ActivityManager.PROCESS_STATE_CACHED_EMPTY);
        maclauncher.android.ProcessAuthority.processStateChanged(ActivityManager.PROCESS_STATE_CACHED_EMPTY);
        HostAppOps.uidStateChanged();
    }
    public static void hostObserveUid(java.util.function.BiConsumer<Integer,Integer> observer){
        int state;
        synchronized(HostActivityManager.class){if(uidObserver!=null)throw new IllegalStateException("Process UID observer is already installed");uidObserver=Objects.requireNonNull(observer);state=started.isEmpty()?ActivityManager.PROCESS_STATE_CACHED_EMPTY:ActivityManager.PROCESS_STATE_TOP;}
        observer.accept(android.os.Process.myUid(),state);
    }
    public static synchronized void hostRetireUidObserver(java.util.function.BiConsumer<Integer,Integer> observer){if(uidObserver==observer)uidObserver=null;}
    public static void hostDirectoryUidChanged(HostProcessServices directory,int uid,int state){
        java.util.function.BiConsumer<Integer,Integer> observer;
        synchronized(HostActivityManager.class){if(processDirectory!=directory)return;observer=uidObserver;}
        if(observer!=null)observer.accept(uid,state);
    }
    private static synchronized int importance(){return started.isEmpty()?RunningAppProcessInfo.IMPORTANCE_CACHED:RunningAppProcessInfo.IMPORTANCE_FOREGROUND;}
    /** AOSP's observer dispatcher queries the process service, not UI state. */
    public static int hostUidProcessState(android.app.ActivityManagerInternal service,int uid){
        if(service!=null)return service.getUidProcessState(uid);
        if(uid!=android.os.Process.myUid()){
            var directory=processDirectory;if(directory!=null)return directory.uidProcessState(uid);
            try{var shared=HostServices.platformService(Context.ACTIVITY_SERVICE);if(shared!=null)return android.app.IActivityManager.Stub.asInterface(shared).getUidProcessState(uid,"android");}
            catch(android.os.RemoteException failure){throw failure.rethrowFromSystemServer();}
            return ActivityManager.PROCESS_STATE_NONEXISTENT;
        }
        synchronized(HostActivityManager.class){return started.isEmpty()?ActivityManager.PROCESS_STATE_CACHED_EMPTY:ActivityManager.PROCESS_STATE_TOP;}
    }
    /** Process capabilities use the same live UID state and explicit device grants. */
    public static int hostUidCapability(Object receiver,int uid){
        if(receiver instanceof android.app.ActivityManagerInternal original)return original.getUidCapability(uid);
        int state=hostUidProcessState(null,uid);if(state<0||state>ActivityManager.PROCESS_STATE_BOUND_TOP)return ActivityManager.PROCESS_CAPABILITY_NONE;
        int result=ActivityManager.PROCESS_CAPABILITY_NONE;
        if(HostPermissions.check(android.Manifest.permission.CAMERA,uid,Context.DEVICE_ID_DEFAULT)==android.content.pm.PackageManager.PERMISSION_GRANTED)result|=ActivityManager.PROCESS_CAPABILITY_FOREGROUND_CAMERA;
        if(HostPermissions.check(android.Manifest.permission.RECORD_AUDIO,uid,Context.DEVICE_ID_DEFAULT)==android.content.pm.PackageManager.PERMISSION_GRANTED)result|=ActivityManager.PROCESS_CAPABILITY_FOREGROUND_MICROPHONE;
        if(HostPermissions.check(android.Manifest.permission.ACCESS_COARSE_LOCATION,uid,Context.DEVICE_ID_DEFAULT)==android.content.pm.PackageManager.PERMISSION_GRANTED||HostPermissions.check(android.Manifest.permission.ACCESS_FINE_LOCATION,uid,Context.DEVICE_ID_DEFAULT)==android.content.pm.PackageManager.PERMISSION_GRANTED)result|=ActivityManager.PROCESS_CAPABILITY_FOREGROUND_LOCATION;
        return result;
    }
    public static boolean hostIsAppForeground(Object receiver,int uid){
        if(receiver instanceof android.app.ActivityManagerInternal service)return service.isAppForeground(uid);
        int state=hostUidProcessState(null,uid);return state>=0&&state<=android.app.ActivityManager.PROCESS_STATE_TOP;
    }
    public static void hostEnforceCallingPermission(Object receiver,String permission,String operation){
        if(receiver instanceof android.app.ActivityManagerInternal service){service.enforceCallingPermission(permission,operation);return;}
        if(HostPermissions.check(permission,android.os.Binder.getCallingUid(),Context.DEVICE_ID_DEFAULT)!=android.content.pm.PackageManager.PERMISSION_GRANTED)
            throw new SecurityException(operation+" requires "+permission);
    }
    /** User selectors and cross-user authority belong to the same hosted user
     * and authenticated process identities as package and account services. */
    public static int hostHandleIncomingUser(Object receiver,int pid,int uid,int user,boolean allowAll,int allowMode,String operation,String caller){
        if(receiver instanceof android.app.ActivityManagerInternal service)return service.handleIncomingUser(pid,uid,user,allowAll,allowMode,operation,caller);
        int callingUser=android.os.UserHandle.getUserId(uid),currentUser=HostApplication.applicationContext().getUserId();
        int result=user==android.os.UserHandle.USER_CURRENT||user==android.os.UserHandle.USER_CURRENT_OR_SELF?currentUser:user;
        if(result<0&&!(allowAll&&result==android.os.UserHandle.USER_ALL))throw new IllegalArgumentException(operation+": invalid Android user "+user);
        if(callingUser==result||uid==android.os.Process.SYSTEM_UID||uid==android.os.Process.ROOT_UID)return result;
        boolean full=HostPermissions.check(android.Manifest.permission.INTERACT_ACROSS_USERS_FULL,uid,Context.DEVICE_ID_DEFAULT)==android.content.pm.PackageManager.PERMISSION_GRANTED;
        // This profile currently hosts one user, so there is no distinct user
        // in the same profile group for the profile-only exemption.
        boolean nonFull=allowMode!=android.app.ActivityManagerInternal.ALLOW_FULL_ONLY&&allowMode!=android.app.ActivityManagerInternal.ALLOW_NON_FULL_IN_PROFILE
            &&HostPermissions.check(android.Manifest.permission.INTERACT_ACROSS_USERS,uid,Context.DEVICE_ID_DEFAULT)==android.content.pm.PackageManager.PERMISSION_GRANTED;
        if(full||nonFull)return result;
        if(user==android.os.UserHandle.USER_CURRENT_OR_SELF)return callingUser;
        throw new SecurityException(operation+": cross-user access requires Android user permission");
    }
    public static int hostCheckContentProviderUriPermission(Object receiver,android.net.Uri uri,int user,int uid,int mode){
        if(receiver instanceof android.app.ActivityManagerInternal service)return service.checkContentProviderUriPermission(uri,user,uid,mode);
        Context context=HostApplication.applicationContext();
        if(user!=context.getUserId())return android.content.pm.PackageManager.PERMISSION_DENIED;
        try(var provider=context.getContentResolver().acquireUnstableContentProviderClient(uri)){
            return provider==null?android.content.pm.PackageManager.PERMISSION_DENIED:provider.checkUriPermission(uri,uid,mode);
        }catch(android.os.RemoteException unavailable){return android.content.pm.PackageManager.PERMISSION_DENIED;}
    }
    public static void getMemoryInfo(MemoryInfo result){
        Objects.requireNonNull(result);long[] values=new long[4];Bridge.memoryInfo(values);
        result.availMem=values[0];result.totalMem=values[1];result.threshold=values[2];result.lowMemory=values[3]!=0;
    }
    public static android.os.Debug.MemoryInfo[] getProcessMemoryInfo(int[] pids){
        Objects.requireNonNull(pids);var results=new android.os.Debug.MemoryInfo[pids.length];
        // ActivityManagerService returns one original MemoryInfo per requested
        // PID, including an empty entry for an inaccessible or exited process.
        // The shared Debug native owner measures this Android process's Mach
        // regions; no other Mac process is inspected by this service.
        for(int i=0;i<pids.length;i++){
            results[i]=new android.os.Debug.MemoryInfo();
            android.os.Debug.getMemoryInfo(pids[i],results[i]);
        }
        return results;
    }
    public static long[] getProcessPss(int[] pids){
        Objects.requireNonNull(pids);long[] results=new long[pids.length];
        for(int i=0;i<pids.length;i++)results[i]=android.os.Debug.getPss(pids[i],null,null);
        return results;
    }
    public static List<RunningAppProcessInfo> getRunningAppProcesses(Context context){
        RunningAppProcessInfo info=new RunningAppProcessInfo(maclauncher.android.framework.HostContext.hostProcessName(),android.os.Process.myPid(),new String[]{context.getPackageName()});
        info.uid=android.os.Process.myUid();info.importance=importance();return new ArrayList<>(List.of(info));
    }
    public static void getMyMemoryState(RunningAppProcessInfo result){
        Objects.requireNonNull(result);result.pid=android.os.Process.myPid();result.uid=android.os.Process.myUid();
        result.processName=maclauncher.android.framework.HostContext.hostProcessName();result.importance=importance();result.lru=0;
        result.importanceReasonCode=0;result.importanceReasonPid=0;result.importanceReasonComponent=null;
    }
}
