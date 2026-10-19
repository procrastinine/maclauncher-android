package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.os.*;
import com.android.server.am.PendingIntentRecord;
import com.android.server.wm.SafeActivityOptions;
import java.lang.ref.WeakReference;

/** OS-service destinations used by the original PendingIntentController and Record. No token state. */
public final class HostPendingIntentDispatch {
    private HostPendingIntentDispatch(){}
    private static final ThreadLocal<IApplicationThread> initiatingApplication=new ThreadLocal<>();
    public static int start(IApplicationThread caller,java.util.function.IntSupplier send){
        IApplicationThread previous=initiatingApplication.get();initiatingApplication.set(caller);
        try{return send.getAsInt();}finally{if(previous==null)initiatingApplication.remove();else initiatingApplication.set(previous);}
    }
    private static Context context(){return HostApplication.applicationContext();}
    public static int startActivityInPackage(Object service,int uid,int pid,int callerUid,String pkg,String feature,Intent intent,String type,IBinder result,String who,int request,int flags,SafeActivityOptions options,int user,Object task,String reason,boolean validate,PendingIntentRecord pending,boolean background){return HostActivityTasks.launchPending(initiatingApplication.get(),pkg,intent,user,result,who,request);}
    public static int startActivitiesInPackage(Object service,int uid,int pid,int callerUid,String pkg,String feature,Intent[] intents,String[] types,IBinder result,SafeActivityOptions options,int user,boolean validate,PendingIntentRecord pending,boolean background){int status=ActivityManager.START_SUCCESS;for(int i=0;i<intents.length;i++){status=HostActivityTasks.launchPending(initiatingApplication.get(),pkg,intents[i],user,i==intents.length-1?result:null,null,-1);if(status<0)break;}return status;}
    public static void sendActivityResult(Object service,int callingUid,IBinder result,String who,int request,int code,Intent intent){new Handler(context().getMainLooper()).post(()->ActivityThread.currentActivityThread().sendActivityResult(result,who,request,code,intent));}
    public static int broadcastIntentInPackage(Object service,String pkg,String feature,int uid,int callerUid,int pid,Intent intent,String type,IApplicationThread caller,IIntentReceiver finished,int code,String data,Bundle extras,String permission,Bundle options,boolean ordered,boolean sticky,int user,BackgroundStartPrivileges privileges,int[] allowList){return maclauncher.android.bridge.LocalBroadcasts.send(context(),caller,intent,type,finished,code,data,extras,permission==null?null:new String[]{permission},null,null,ordered,sticky,user);}
    public static ComponentName startServiceInPackage(Object service,int uid,Intent intent,String type,boolean foreground,String pkg,String feature,int user,BackgroundStartPrivileges privileges){return foreground?context().startForegroundService(intent):context().startService(intent);}
    public static int getCurrentOrTargetUserId(Object service){return context().getUserId();}
    public static int getUidProcessState(Object service,int uid){return HostActivityManager.hostUidProcessState(null,uid);}
    public static boolean isUidForeground(Object service,int uid){return getUidProcessState(service,uid)<=ActivityManager.PROCESS_STATE_FOREGROUND_SERVICE;}
    public static void remove(Object service,PendingIntent pending){context().getSystemService(AlarmManager.class).cancel(pending);}
    public static void clearPendingResultForActivity(Object service,IBinder activity,WeakReference<PendingIntentRecord> pending){
        // HostActivity retains only its live ActivityThread record. There is no
        // second pending-result list: cancellation has already removed the
        // original controller's weak reference before reaching this boundary.
    }
    public static void tempAllowlistForPendingIntent(Object service,int pid,int uid,int target,long duration,int type,int reason,String tag){
        // The host does not suspend Android processes under Android device-idle
        // policy. macOS owns process scheduling; an Android temporary exemption
        // therefore needs no second timer or power-policy owner.
    }
    public static void enforceBroadcastOptionsPermissions(Object service,Bundle options,int uid){
        if(options!=null&&new BroadcastOptions(options).isAlarmBroadcast()&&uid!=android.os.Process.SYSTEM_UID&&uid!=android.os.Process.ROOT_UID)throw new SecurityException("Alarm-originated broadcast requires the system UID");
    }
    public static void scheduleUpdateBinderHeavyHitterWatcherConfig(Object service){
        // This callback configures system_server's Binder watchdog. Host Binder
        // RPC diagnostics belong to the RPC transport; no system_server exists.
    }
}
