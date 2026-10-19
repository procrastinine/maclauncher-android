package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.os.*;
import android.provider.Settings;
import android.widget.Switch;
import java.util.*;

/** Platform storage settings for the existing private Android storage volume. */
final class HostStorageSettings {
    private static final String PERMISSION=android.Manifest.permission.MANAGE_EXTERNAL_STORAGE;
    private static final String DECISION="appop:android:manage_external_storage";
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
        Handler main=new Handler(Looper.getMainLooper());
        return main.post(()->{
            Activity activity=HostApplication.current().thread.getActivity(token);
            if(activity==null||activity.isDestroyed()||activity.isFinishing())return;
            Session existing=windows.get(token);if(existing!=null){existing.results.add(new Result(caller,who,request));existing.dialog.show();return;}
            new Session(activity,caller,token,who,request).show();
        })?ActivityManager.START_SUCCESS:ActivityManager.START_CANCELED;
    }
    private record Result(IApplicationThread caller,String who,int request){}
    private static final class Session implements Application.ActivityLifecycleCallbacks {
        final Activity activity;final IBinder token;final List<Result> results=new ArrayList<>();
        final Runnable shutdown=this::close;
        AlertDialog dialog;AutoCloseable lifecycle;boolean closed;
        Session(Activity activity,IApplicationThread caller,IBinder token,String who,int request){this.activity=activity;this.token=token;results.add(new Result(caller,who,request));}
        void show(){
            Switch access=new Switch(activity);access.setText("Allow access to manage all files");
            access.setChecked(mode()==AppOpsManager.MODE_ALLOWED);access.setEnabled(HostPermissions.requested(PERMISSION));
            int padding=Math.round(24*activity.getResources().getDisplayMetrics().density);access.setPadding(padding,padding,padding,padding);
            access.setOnCheckedChangeListener((button,allowed)->{
                if(!HostPermissions.requested(PERMISSION))return;
                HostPermissions.decisions().edit().putInt(DECISION,allowed?AppOpsManager.MODE_ALLOWED:AppOpsManager.MODE_ERRORED).apply();
                HostAppOps.permissionsChanged();
            });
            dialog=new AlertDialog.Builder(activity).setTitle("All files access")
                .setMessage("Access applies to this application's Android storage.")
                .setView(access).setPositiveButton(android.R.string.ok,(value,which)->{}).create();
            dialog.setOnDismissListener(value->close());
            lifecycle=HostActivity.platformDialog(activity);windows.put(token,this);
            activity.getApplication().registerActivityLifecycleCallbacks(this);
            if(!HostLooper.addShutdownListener(Looper.getMainLooper(),shutdown)){close();return;}
            try{dialog.show();}catch(RuntimeException|Error failure){close();throw failure;}
        }
        void close(){
            if(closed)return;closed=true;windows.remove(token,this);
            HostLooper.removeShutdownListener(Looper.getMainLooper(),shutdown);
            activity.getApplication().unregisterActivityLifecycleCallbacks(this);
            if(dialog!=null)dialog.dismiss();
            try{
                if(!activity.isDestroyed()&&!activity.isFinishing())for(Result result:results)HostActivity.deliverResult(result.caller,token,result.who,result.request,Activity.RESULT_CANCELED,null);
            }finally{
                results.clear();
                AutoCloseable owned=lifecycle;lifecycle=null;
                // sendActivityResult queues the original ActivityResultItem.
                // Let it run while the caller is paused, before restoring the
                // foreground lifecycle; otherwise it introduces a second
                // artificial pause/resume around an already resumed Activity.
                Runnable resume=()->{
                    if(owned!=null)try{owned.close();}catch(RuntimeException|Error failure){throw failure;}catch(Exception failure){throw new IllegalStateException("Android settings window could not close",failure);}
                };
                if(!new Handler(Looper.getMainLooper()).post(resume))resume.run();
            }
        }
        @Override public void onActivityDestroyed(Activity value){if(value==activity)close();}
        @Override public void onActivityCreated(Activity value,Bundle state){}
        @Override public void onActivityStarted(Activity value){}
        @Override public void onActivityResumed(Activity value){}
        @Override public void onActivityPaused(Activity value){}
        @Override public void onActivityStopped(Activity value){}
        @Override public void onActivitySaveInstanceState(Activity value,Bundle state){}
    }
}
