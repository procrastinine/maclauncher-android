package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.content.pm.ActivityInfo;
import android.content.pm.ConfigurationInfo;
import android.os.*;
import java.util.*;
import com.android.server.am.HostPendingIntentInjector;

/** macOS activity destinations below the original Context and Instrumentation clients. */
final class HostActivityTasks extends IActivityTaskManager.Default {
    private final Context context;
    private AppTask appTask;
    private static final ThreadLocal<Bundle> initiatingWindow=new ThreadLocal<>();
    @FunctionalInterface interface WindowRequest {int run(Bundle options)throws RemoteException;}
    static int windowRequest(Bundle options,WindowRequest request)throws RemoteException{
        Bundle windowOptions=HostActivity.windowOptions(options),previous=initiatingWindow.get();initiatingWindow.set(windowOptions);
        try{return request.run(windowOptions);}finally{if(previous==null)initiatingWindow.remove();else initiatingWindow.set(previous);}
    }
    private HostActivityTasks(Context context){this.context=context;}
    private static void cancelResult(IApplicationThread caller,IBinder result,String who,int request){new Handler(Looper.getMainLooper()).post(()->HostActivity.deliverResult(caller,result,who,request,Activity.RESULT_CANCELED,null));}
    static void install(Context context){HostServices.register(Context.ACTIVITY_TASK_SERVICE,new HostActivityTasks(context),"android.app.IActivityTaskManager");}
    @Override public synchronized List<IBinder> getAppTasks(String packageName){
        var peer=HostBinderRpc.callingPeer();String caller=peer==null?context.getPackageName():peer.packageName();
        if(!Objects.equals(caller,packageName))throw new SecurityException("Task caller does not match the calling process");
        int id=HostActivity.currentTaskId();
        if(id<0||!Objects.equals(packageName,context.getPackageName())){appTask=null;return List.of();}
        if(appTask==null||appTask.id!=id)appTask=new AppTask(id);
        return List.of(HostServiceEndpoints.export(appTask,"android.app.IAppTask",context));
    }
    /** The task capability reads the existing lifecycle owner; it keeps no Activity copies. */
    private final class AppTask extends IAppTask.Default {
        final int id;AppTask(int id){this.id=id;}
        @Override public ActivityManager.RecentTaskInfo getTaskInfo(){var info=HostActivity.taskInfo(id);if(info==null)throw new IllegalArgumentException("Activity task no longer exists");return info;}
        @Override public void finishAndRemoveTask(){HostActivity.finishTask(id);}
        @Override public void moveToFront(IApplicationThread caller,String packageName){getTaskInfo();HostActivity.resumeTask(id);}
        @Override public int startActivity(IBinder caller,String packageName,String feature,Intent intent,String type,Bundle options){
            getTaskInfo();return launch(IApplicationThread.Stub.asInterface(caller),packageName,intent,context.getUserId(),null,null,-1,options);
        }
        @Override public void setExcludeFromRecents(boolean exclude){HostActivity.excludeTask(id,exclude);}
    }
    private static boolean permitted(ActivityInfo info,int uid){return ActivityManager.checkComponentPermission(info.permission,uid,info.applicationInfo.uid,info.exported)==android.content.pm.PackageManager.PERMISSION_GRANTED;}
    static boolean permitsUp(Intent intent){
        var context=HostApplication.applicationContext();
        var resolved=context.getPackageManager().resolveActivity(intent,android.content.pm.PackageManager.MATCH_DEFAULT_ONLY);
        return resolved!=null&&resolved.activityInfo!=null&&permitted(resolved.activityInfo,android.os.Process.myUid())&&HostGoogleProviders.accountActivityAllowed(resolved.activityInfo);
    }
    /** Up runs after its captured task request reaches the existing main owner. */
    static int launchUp(Intent intent,IApplicationThread caller,IBinder parent){
        var context=HostApplication.applicationContext();
        return new HostActivityTasks(context).launch(caller,context.getPackageName(),intent,context.getUserId(),parent,null,-1,null,true);
    }
    private int launch(IApplicationThread caller,String pkg,Intent intent,int user,IBinder result,String who,int request,Bundle options){return launch(caller,pkg,intent,user,result,who,request,options,false);}
    private int launch(IApplicationThread caller,String pkg,Intent intent,int user,IBinder result,String who,int request,Bundle options,boolean up){
        var peer=HostBinderRpc.callingPeer();String callerPackage=peer==null?context.getPackageName():peer.packageName();
        if(!Objects.equals(pkg,callerPackage)||user!=context.getUserId())return ActivityManager.START_PERMISSION_DENIED;
        Objects.requireNonNull(intent);if(HostPermissions.handles(intent))return HostPermissions.request(intent,result,who,request);
        if(HostStorageSettings.handles(intent))return HostStorageSettings.request(context,intent,caller,result,who,request);
        var resolved=context.getPackageManager().resolveActivity(intent,android.content.pm.PackageManager.MATCH_DEFAULT_ONLY);
        if(resolved!=null&&resolved.activityInfo!=null){
            var info=resolved.activityInfo;String process=info.processName==null?info.applicationInfo.processName:info.processName;
            int callerUid=peer==null?android.os.Process.myUid():peer.uid(),callerPid=peer==null?android.os.Process.myPid():peer.pid();
            if(!permitted(info,callerUid))return ActivityManager.START_PERMISSION_DENIED;
            if(!HostGoogleProviders.accountActivityAllowed(info)){
                cancelResult(caller,result,who,request);return ActivityManager.START_SUCCESS;
            }
            Intent launch=new Intent(intent).setComponent(new ComponentName(info.packageName,info.name));
            if(Objects.equals(info.packageName,context.getPackageName())&&Objects.equals(process,HostContext.hostProcessName())){
                if(up)HostActivity.launchUp(info,launch,caller,result);
                else HostActivity.launch(info,launch,caller,result,who,request,options);
                return ActivityManager.START_SUCCESS;
            }
            // The receiving process authenticates this process and owns its own
            // Application/ActivityThread. Original Binder arguments retain the
            // caller's actual result destination through the transport relay.
            var destination=HostRemoteServices.activityTasksAsync(info);
            if(destination==null)return ActivityManager.START_CANCELED;
            String resolvedType=launch.resolveTypeIfNeeded(context.getContentResolver());
            Bundle windowOptions=HostActivity.windowOptions(options);
            destination.whenComplete((manager,failure)->{
                if(failure!=null||manager==null){if(failure!=null)android.util.Log.w("ActivityTaskManager","Android activity process could not start",failure);cancelResult(caller,result,who,request);return;}
                try{if(manager.startActivityAsUser(caller,context.getPackageName(),null,launch,resolvedType,result,who,request,0,null,windowOptions,user)<0)cancelResult(caller,result,who,request);}
                catch(RemoteException|RuntimeException unavailable){android.util.Log.w("ActivityTaskManager","Android activity could not start",unavailable);cancelResult(caller,result,who,request);}
            });
            return ActivityManager.START_SUCCESS;
        }
        try{maclauncher.android.bridge.WebLinks.open(intent);return ActivityManager.START_SUCCESS;}catch(ActivityNotFoundException missing){return ActivityManager.START_INTENT_NOT_RESOLVED;}
    }
    static int launchPending(IApplicationThread caller,String pkg,Intent intent,int user,IBinder result,String who,int request){return new HostActivityTasks(HostApplication.applicationContext()).launch(caller,pkg,intent,user,result,who,request,initiatingWindow.get());}
    @Override public int startActivityIntentSender(IApplicationThread caller,IIntentSender sender,IBinder allowlist,Intent fill,String type,IBinder result,String who,int request,int mask,int flags,Bundle options)throws RemoteException{
        return windowRequest(options,windowOptions->{var remote=HostRemoteServices.intentTasks(sender);return remote==null?HostPendingIntentInjector.start(caller,sender,allowlist,fill,type,result,who,request,mask,flags,options):remote.startActivityIntentSender(caller,sender,allowlist,fill,type,result,who,request,mask,flags,windowOptions);});
    }
    @Override public int startActivity(IApplicationThread caller,String pkg,String feature,Intent intent,String type,IBinder result,String who,int request,int flags,ProfilerInfo profiler,Bundle options){return launch(caller,pkg,intent,context.getUserId(),result,who,request,options);}
    @Override public int startActivityAsUser(IApplicationThread caller,String pkg,String feature,Intent intent,String type,IBinder result,String who,int request,int flags,ProfilerInfo profiler,Bundle options,int user){return launch(caller,pkg,intent,user,result,who,request,options);}
    @Override public int startActivities(IApplicationThread caller,String pkg,String feature,Intent[] intents,String[] types,IBinder result,Bundle options,int user){int status=ActivityManager.START_SUCCESS;for(Intent intent:Objects.requireNonNull(intents)){status=launch(caller,pkg,intent,user,result,null,-1,options);if(status<0)break;}return status;}
    @Override public IActivityClientController getActivityClientController(){return IActivityClientController.Stub.asInterface(HostServiceEndpoints.export(HostActivity.clientController(),"android.app.IActivityClientController",context));}
    static void cancelRequest(Context context,Intent intent,IBinder token)throws RemoteException {
        var resolved=context.getPackageManager().resolveActivity(intent,android.content.pm.PackageManager.MATCH_DEFAULT_ONLY);
        if(resolved==null)return;var info=resolved.activityInfo;
        String process=info.processName==null?info.applicationInfo.processName:info.processName;
        if(Objects.equals(info.packageName,context.getPackageName())&&Objects.equals(process,HostContext.hostProcessName()))HostActivity.finishChildren(token,null,0);
        else {var destination=HostRemoteServices.activityTasksAsync(info);if(destination!=null)destination.whenComplete((manager,failure)->{if(failure!=null)return;try{if(manager!=null)manager.getActivityClientController().finishSubActivity(token,null,0);}catch(RemoteException|RuntimeException unavailable){android.util.Log.w("ActivityTaskManager","Android activity request could not close",unavailable);}});}
    }
    @Override public ConfigurationInfo getDeviceConfigurationInfo(){return HostActivityManager.getDeviceConfigurationInfo(context);}
}
