package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.content.pm.ActivityInfo;
import android.os.*;
import android.provider.Settings;
import android.widget.Switch;
import java.util.*;

/** Platform storage settings for the existing private Android storage volume. */
final class HostStorageSettings {
    private static final String PERMISSION=android.Manifest.permission.MANAGE_EXTERNAL_STORAGE;
    private static final String DECISION="appop:android:manage_external_storage";
    private static final String SESSION="maclauncher.android.storage-settings";
    private static final Map<IBinder,Session> windows=new IdentityHashMap<>();
    private HostStorageSettings(){}
    static int mode(){
        if(!HostPermissions.requested(PERMISSION))return AppOpsManager.MODE_DEFAULT;
        int value=HostPermissions.decisions().getInt(DECISION,AppOpsManager.MODE_DEFAULT);
        return value==AppOpsManager.MODE_ALLOWED||value==AppOpsManager.MODE_ERRORED?value:AppOpsManager.MODE_DEFAULT;
    }
    static boolean handles(Intent intent){
        return intent.getComponent()==null&&intent.getPackage()==null&&
            (Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION.equals(intent.getAction())||Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION.equals(intent.getAction()));
    }
    static int request(Context context,Intent intent,IApplicationThread caller,IBinder token,String who,int request){
        if(token==null)return ActivityManager.START_PERMISSION_DENIED;
        if(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION.equals(intent.getAction())){
            var data=intent.getData();
            if(data==null||!"package".equals(data.getScheme())||!context.getPackageName().equals(data.getSchemeSpecificPart()))return ActivityManager.START_PERMISSION_DENIED;
        }
        synchronized(windows){
            for(var existing:windows.values())if(existing.callerToken==token){existing.results.add(new Result(caller,who,request));return ActivityManager.START_SUCCESS;}
            var session=new Session(token,caller,who,request);windows.put(session.capability,session);
            var info=new ActivityInfo();info.applicationInfo=context.getApplicationInfo();info.packageName=context.getPackageName();
            info.name=StorageActivity.class.getName();info.processName=HostContext.hostProcessName();info.taskAffinity=info.packageName+".platform.settings";
            info.theme=android.R.style.Theme_Material_Light_NoActionBar;info.exported=false;
            Intent launch=new Intent(intent).setComponent(new ComponentName(info.packageName,info.name)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            Bundle extras=new Bundle();extras.putBinder(SESSION,session.capability);launch.putExtras(extras);
            try{HostActivity.launchPlatform(info,launch,caller,token,who);}
            catch(RuntimeException|Error failure){windows.remove(session.capability);throw failure;}
        }
        return ActivityManager.START_SUCCESS;
    }
    private record Result(IApplicationThread caller,String who,int request){}
    /** ActivityThread, PhoneWindow and the original main Looper own this UI. */
    public static final class StorageActivity extends Activity {
        private Session session;
        @Override protected void onCreate(Bundle state){
            super.onCreate(state);
            IBinder capability=getIntent().getExtras()==null?null:getIntent().getExtras().getBinder(SESSION);
            synchronized(windows){session=windows.get(capability);}
            if(session==null)throw new IllegalStateException("Android storage settings request is no longer active");
            setTitle("All files access");session.show(this);
        }
        @Override protected void onDestroy(){
            try{if(session!=null){if(isChangingConfigurations())session.detach(this);else session.close(this);}}
            finally{super.onDestroy();}
        }
    }
    private static final class Session {
        final IBinder capability=new Binder(),callerToken;
        final List<Result> results=new ArrayList<>();
        StorageActivity activity;AlertDialog dialog;boolean closed;
        Session(IBinder token,IApplicationThread caller,String who,int request){callerToken=token;results.add(new Result(caller,who,request));}
        void show(StorageActivity owner){
            activity=owner;
            Switch access=new Switch(owner);access.setText("Allow access to manage all files");
            access.setChecked(mode()==AppOpsManager.MODE_ALLOWED);access.setEnabled(HostPermissions.requested(PERMISSION));
            int padding=Math.round(24*owner.getResources().getDisplayMetrics().density);access.setPadding(padding,padding,padding,padding);
            access.setOnCheckedChangeListener((button,allowed)->{
                if(!HostPermissions.requested(PERMISSION))return;
                HostPermissions.decisions().edit().putInt(DECISION,allowed?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_ERRORED).apply();
                HostAppOps.permissionsChanged();
            });
            dialog=new AlertDialog.Builder(owner).setTitle("All files access")
                .setMessage("Access applies to this application's Android storage.")
                .setView(access).setPositiveButton(android.R.string.ok,(value,which)->{}).create();
            dialog.setOwnerActivity(owner);
            dialog.setOnDismissListener(value->{if(activity==owner&&!owner.isDestroyed()&&!owner.isChangingConfigurations())owner.finish();});
            dialog.show();
        }
        void detach(StorageActivity owner){
            if(activity!=owner)return;activity=null;
            if(dialog!=null){dialog.setOnDismissListener(null);dialog.dismiss();dialog=null;}
        }
        void close(StorageActivity owner){
            if(closed)return;closed=true;detach(owner);
            synchronized(windows){windows.remove(capability);}
            try{for(Result result:results)HostActivity.deliverResult(result.caller,callerToken,result.who,result.request,Activity.RESULT_CANCELED,null);}
            finally{results.clear();}
        }
    }
}
