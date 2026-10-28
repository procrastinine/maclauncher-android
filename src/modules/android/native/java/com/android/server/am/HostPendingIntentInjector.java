package com.android.server.am;

import android.app.*;
import android.content.*;
import android.os.*;
import com.android.internal.os.IResultReceiver;

/** Process-host lifetime for Android's original pending-intent owner. */
public final class HostPendingIntentInjector implements AutoCloseable {
    private final Context context;
    private final PendingIntentController controller;
    private final Runnable retirement=this::close;
    public HostPendingIntentInjector(Context context){
        this.context=context;
        var constants=new ActivityManagerConstants(context,null,new Handler(context.getMainLooper()));
        controller=new PendingIntentController(context.getMainLooper(),null,constants);
        controller.onActivityManagerInternalAdded();
        // Retire original tokens before later queue listeners unpublish the
        // AlarmManager used by PendingIntentController cancellation.
        maclauncher.android.framework.HostLooper.addShutdownListener(context.getMainLooper(),retirement);
    }
    public IIntentSender create(int type,String pkg,String feature,int uid,int user,IBinder activity,String who,int request,Intent[] intents,String[] types,int flags,Bundle options){
        return controller.getIntentSender(type,pkg,feature,uid,user,activity,who,request,intents,types,flags,options);
    }
    public void cancel(IIntentSender sender){controller.cancelIntentSender(sender);}
    public boolean register(IIntentSender sender,IResultReceiver receiver){return controller.registerIntentSenderCancelListener(sender,receiver);}
    public void unregister(IIntentSender sender,IResultReceiver receiver){controller.unregisterIntentSenderCancelListener(sender,receiver);}
    public static ActivityManager.PendingIntentInfo info(IIntentSender sender){
        if(!(sender instanceof PendingIntentRecord record))return null;
        return new ActivityManager.PendingIntentInfo(record.key.packageName,record.uid,(record.key.flags&PendingIntent.FLAG_IMMUTABLE)!=0,record.key.type);
    }
    public static Intent intent(IIntentSender sender){return sender instanceof PendingIntentRecord record&&record.key.requestIntent!=null?new Intent(record.key.requestIntent):null;}
    public static boolean activity(IIntentSender sender){return sender instanceof PendingIntentRecord record&&record.key.type==ActivityManager.INTENT_SENDER_ACTIVITY;}
    public static int send(IApplicationThread caller,IIntentSender sender,IBinder allowlist,int code,Intent intent,String type,IIntentReceiver finished,String permission,Bundle options)throws RemoteException{
        if(sender instanceof PendingIntentRecord record)return record.sendWithResult(caller,code,intent,type,allowlist,finished,permission,options);
        // IIntentSender can also be an application-provided Binder, with its own
        // delivery contract. Do not turn a remote Binder into another local token.
        sender.send(code,intent==null?new Intent(Intent.ACTION_MAIN):intent,type,allowlist,finished,permission,options);
        return ActivityManager.START_SUCCESS;
    }
    public static int start(IApplicationThread caller,IIntentSender sender,IBinder allowlist,Intent fill,String type,IBinder result,String who,int request,int mask,int flags,Bundle options){
        if(!(sender instanceof PendingIntentRecord record))return ActivityManager.START_CANCELED;
        return maclauncher.android.framework.HostPendingIntentDispatch.start(caller,()->record.sendInner(caller,0,fill,type,allowlist,null,null,result,who,request,mask,flags,options));
    }
    @Override public void close(){controller.removePendingIntentsForPackage(context.getPackageName(),context.getUserId(),UserHandle.getAppId(android.os.Process.myUid()),true,PendingIntentRecord.CANCEL_REASON_OWNER_FORCE_STOPPED);maclauncher.android.framework.HostLooper.removeShutdownListener(context.getMainLooper(),retirement);}
}
