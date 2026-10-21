package maclauncher.android;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import android.provider.Settings;
import android.view.*;
import android.widget.*;
import java.util.*;
import maclauncher.android.framework.*;

/** Original Environment, settings controls, AppOps callbacks and Activity return. */
public final class StorageSettingsProbe {
    private static void check(boolean value,String text){if(!value)throw new AssertionError(text);}
    private static void pumpUntil(Looper loop,java.util.function.BooleanSupplier done)throws Exception {
        long deadline=System.nanoTime()+2_000_000_000L;
        do{HostWindowProbe.pump(loop);Thread.sleep(2);}while(!done.getAsBoolean()&&System.nanoTime()<deadline);
        check(done.getAsBoolean(),"settings operation completed on original Looper");
    }
    private static AlertDialog dialog()throws Exception {
        var field=Class.forName("maclauncher.android.framework.HostStorageSettings").getDeclaredField("windows");field.setAccessible(true);
        var windows=(Map<?,?>)field.get(null);if(windows.isEmpty())return null;
        Object owner=windows.values().iterator().next();var value=owner.getClass().getDeclaredField("dialog");value.setAccessible(true);return (AlertDialog)value.get(owner);
    }
    private static CompoundButton toggle(View view){
        if(view instanceof CompoundButton button)return button;
        if(view instanceof ViewGroup group)for(int i=0;i<group.getChildCount();i++){var found=toggle(group.getChildAt(i));if(found!=null)return found;}
        return null;
    }
    public static void run(Activity base,Looper loop)throws Exception {
        var activity=(HostWindowProbe.OwnedActivity)base;Thread main=Thread.currentThread();
        if(Boolean.getBoolean("android.fixture.storageFinish")){finishingCaller(activity,loop);return;}
        check(!Environment.isExternalStorageManager(),"all-files access is initially denied");
        var files=Environment.getExternalStorageDirectory();
        var appOps=activity.getSystemService(AppOpsManager.class);String pkg=activity.getPackageName();int uid=android.os.Process.myUid();
        int[] changed={0},paused={0},resumed={0},foreignCreated={0};Thread[] callback={null};boolean[] grantedOnResume={false};
        AppOpsManager.OnOpChangedListener observer=(op,name)->{changed[0]++;callback[0]=Thread.currentThread();};
        appOps.startWatchingMode(AppOpsManager.OPSTR_MANAGE_EXTERNAL_STORAGE,pkg,observer);
        Application.ActivityLifecycleCallbacks lifecycle=new Application.ActivityLifecycleCallbacks(){
            public void onActivityPaused(Activity value){if(value==activity){check(Thread.currentThread()==main,"pause thread");paused[0]++;}}
            public void onActivityResumed(Activity value){if(value==activity){check(Thread.currentThread()==main,"resume thread");resumed[0]++;grantedOnResume[0]=Environment.isExternalStorageManager();}}
            public void onActivityCreated(Activity value,Bundle state){if(value!=activity)foreignCreated[0]++;}public void onActivityStarted(Activity value){}public void onActivityStopped(Activity value){}public void onActivitySaveInstanceState(Activity value,Bundle state){}public void onActivityDestroyed(Activity value){}
        };
        activity.getApplication().registerActivityLifecycleCallbacks(lifecycle);
        try{
            Intent settings=new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,Uri.fromParts("package",pkg,null));
            java.util.concurrent.CompletableFuture.runAsync(()->activity.startActivityForResult(settings,93)).get();
            check(activity.activityRequest==-1&&dialog()==null,"worker request queues the platform window and result");
            pumpUntil(loop,()->{try{return dialog()!=null&&dialog().isShowing();}catch(Exception error){throw new RuntimeException(error);}});
            check(paused[0]==1&&resumed[0]==0,"original Activity pauses while settings is visible");
            AlertDialog first=dialog();CompoundButton access=toggle(first.getWindow().getDecorView());
            check(first.getOwnerActivity().getApplication()!=activity.getApplication()&&foreignCreated[0]==0,"settings uses its own original Application without game lifecycle callbacks");
            check(access!=null&&access.isEnabled()&&!access.isChecked(),"real Android toggle requires a declared permission and starts off");
            check(!Environment.isExternalStorageManager(),"opening settings does not grant access");
            access.performClick();
            check(Environment.isExternalStorageManager()&&appOps.checkOpNoThrow(AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE,uid,pkg)==AppOpsManager.MODE_ALLOWED,"actual toggle updates original Environment through AppOps");
            pumpUntil(loop,()->changed[0]>0);check(callback[0]==main,"original AppOps callback uses main Looper");
            first.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
            pumpUntil(loop,()->activity.activityRequest==93);
            check(resumed[0]==1,"one settings return resumes once: pauses="+paused[0]+" resumes="+resumed[0]);
            check(activity.activityResult==Activity.RESULT_CANCELED&&activity.activityData==null&&activity.activityResultThread==main,"settings returns its ordinary result on Activity main thread");
            check(grantedOnResume[0],"original onResume recheck observes the selected setting");
            android.app.QueuedWork.waitToFinish();
            var preferences=activity.createDeviceProtectedStorageContext().getSharedPreferences("maclauncher.platform.permission-decisions",Context.MODE_PRIVATE);
            check(preferences.getInt("appop:android:manage_external_storage",-1)==AppOpsManager.MODE_ALLOWED,"choice is persisted by the existing Android decision owner");
            var preferenceFile=activity.createDeviceProtectedStorageContext().getSharedPreferencesPath("maclauncher.platform.permission-decisions").toPath();
            check(java.nio.file.Files.readString(preferenceFile).contains("appop:android:manage_external_storage"),"original preference writer records the decision on disk");
            check(Environment.getExternalStorageDirectory().equals(files),"settings never changes the storage namespace");
            activity.startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),94);
            pumpUntil(loop,()->{try{return dialog()!=null&&dialog().isShowing();}catch(Exception error){throw new RuntimeException(error);}});
            AlertDialog second=dialog();CompoundButton retained=toggle(second.getWindow().getDecorView());
            check(retained.isChecked(),"generic settings route displays the retained decision");retained.performClick();
            check(!Environment.isExternalStorageManager(),"toggle revokes through the same original Environment query");
            second.dismiss();pumpUntil(loop,()->activity.activityRequest==94&&resumed[0]==2);
            check(!grantedOnResume[0]&&paused[0]==2,"return resumes with the revoked decision");
            try{activity.startActivityForResult(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,Uri.fromParts("package","another.package",null)),95);throw new AssertionError("foreign settings request accepted");}catch(SecurityException expected){}
            check(dialog()==null&&!Environment.isExternalStorageManager(),"another package cannot change this installation's setting");
            System.out.println("PASS original Environment storage toggle, persisted AppOps, main-loop result and resume recheck");
        }finally{appOps.stopWatchingMode(observer);activity.getApplication().unregisterActivityLifecycleCallbacks(lifecycle);AlertDialog showing=dialog();if(showing!=null)showing.dismiss();}
    }
    private static void finishingCaller(HostWindowProbe.OwnedActivity caller,Looper loop)throws Exception {
        HostActivity windowOwner=HostActivity.owner(caller);int callerTask=caller.getTaskId();
        boolean removeTask=Boolean.getBoolean("android.fixture.storageTaskRemove");
        var callerCapability=caller.getSystemService(ActivityManager.class).getAppTasks().stream().filter(task->task.getTaskInfo().taskId==callerTask).findFirst().orElseThrow();
        check(!Environment.isExternalStorageManager(),"all-files access starts denied");
        Intent intent=new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,Uri.fromParts("package",caller.getPackageName(),null));
        caller.startActivityForResult(intent,96);
        if(removeTask)callerCapability.finishAndRemoveTask();else caller.finishAffinity();
        if(!removeTask)check(caller.isFinishing(),"original finishAffinity marks its caller finishing immediately");
        pumpUntil(loop,()->{try{return caller.isDestroyed()&&dialog()!=null&&dialog().isShowing();}catch(Exception failure){throw new RuntimeException(failure);}});
        AlertDialog settings=dialog();Activity destination=settings.getOwnerActivity();
        check(destination!=null&&destination!=caller&&!destination.isDestroyed(),"settings owns an independent original Activity after caller destruction");
        check(HostWindowManager.topView(destination)==settings.getWindow().getDecorView(),"settings dialog stays above its original Activity window");
        check(destination.getTaskId()!=callerTask,"settings has an independent task identity");
        check(!windowOwner.closing(),"the settings task retains its actual display lifetime");
        check(destination.getSystemService(ActivityManager.class).getAppTasks().stream().anyMatch(task->task.getTaskInfo().taskId==destination.getTaskId()),"settings has an owned original AppTask capability");
        check(!Environment.isExternalStorageManager(),"finishing the caller never grants access");
        CompoundButton access=toggle(settings.getWindow().getDecorView());
        check(access!=null&&!access.isChecked(),"surviving consent uses the real off-by-default control");
        access.performClick();check(Environment.isExternalStorageManager(),"explicit surviving consent updates the existing scoped AppOps decision");
        if(removeTask)windowOwner.finish();
        else destination.getSystemService(ActivityManager.class).getAppTasks().stream().filter(task->task.getTaskInfo().taskId==destination.getTaskId()).findFirst().orElseThrow().finishAndRemoveTask();
        pumpUntil(loop,()->destination.isDestroyed()&&windowOwner.closing());
        check(caller.activityRequest==-1,"a destroyed caller receives no synthetic Activity result");
        check(destination.getSystemService(ActivityManager.class).getAppTasks().isEmpty(),"finishing Settings retires all task capabilities");
        System.out.println("PASS independent original Settings task survives caller "+(removeTask?"task removal with whole-window close":"finishAffinity with settings task removal")+", requires consent and retires normally");
    }
}
