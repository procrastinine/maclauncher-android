package maclauncher.android;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import java.util.*;
import java.util.function.BooleanSupplier;
import maclauncher.android.framework.*;

/** Original Activity Up requests through the installed task and lifecycle owners. */
public final class ActivityUpNavigationProbe {
    private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    private static void settle(Looper loop,BooleanSupplier complete)throws Exception{
        long until=System.nanoTime()+2_000_000_000L;
        do{HostWindowProbe.pump(loop);if(complete.getAsBoolean())return;Thread.sleep(2);}while(System.nanoTime()<until);
        check(complete.getAsBoolean(),"original queued Up operation completes");
    }
    private static HostWindowProbe.NextActivity child(Activity parent,Looper loop)throws Exception{
        parent.startActivityForResult(new Intent(parent,HostWindowProbe.NextActivity.class),91);
        HostWindowProbe.pump(loop);return HostWindowProbe.NextActivity.instance;
    }
    public static void run(Activity initial,Looper loop)throws Exception{
        var parent=(HostWindowProbe.OwnedActivity)initial;
        int task=parent.getTaskId();var nested=child(parent,loop);
        Intent destination=new Intent(parent,HostWindowProbe.OwnedActivity.class)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP).setAction("org.example.UP")
            .setData(Uri.parse("content://org.example.window/up")).putExtra("payload","kept");
        nested.setResult(Activity.RESULT_OK,new Intent().putExtra("result","returned"));
        parent.upEvents.clear();
        check(nested.navigateUpTo(destination),"original navigateUpTo accepts an existing parent");
        settle(loop,()->parent.isResumed()&&parent.upIntent!=null);
        check(nested.isDestroyed()&&!parent.isDestroyed(),"Up destroys the child and retains its reusable parent");
        check(parent.getTaskId()==task&&parent.isResumed(),"Up resumes the original parent in the same task");
        check(parent.upIntent!=null&&parent.upIntent.filterEquals(destination)&&"kept".equals(parent.upIntent.getStringExtra("payload")),"original onNewIntent retains destination action/data/extras");
        check(parent.activityRequest==91&&parent.activityResult==Activity.RESULT_OK&&"returned".equals(parent.activityData.getStringExtra("result")),"Up delivers the child's original Activity result");
        check(nested.finishingCallbacks.containsAll(List.of("pause:true","stop:true","destroy:true")),"Up preserves original finishing lifecycle callbacks");
        check(parent.upEvents.indexOf("intent")<parent.upEvents.lastIndexOf("resume"),"original new Intent precedes parent resume");
        System.out.println("PASS original Up navigation: parent reuse, Intent, result and finishing lifecycle");

        var middle=child(parent,loop);nested=child(middle,loop);
        nested.setResult(Activity.RESULT_OK,new Intent().putExtra("first-only",true));
        check(nested.navigateUpTo(destination),"Up finds an ancestor below intervening records");settle(loop,()->parent.isResumed()&&parent.activityResult==Activity.RESULT_CANCELED);
        check(nested.isDestroyed()&&middle.isDestroyed()&&!parent.isDestroyed(),"Up removes the original intermediate Activity records");
        check(parent.activityRequest==91&&parent.activityResult==Activity.RESULT_CANCELED&&parent.activityData==null,"only the first finished Activity uses the supplied result");
        check(!nested.navigateUpTo(destination),"a retired token cannot navigate a surviving task");
        System.out.println("PASS original Up navigation: intervening records, first result and stale token");

        nested=child(parent,loop);
        check(!nested.navigateUpTo(new Intent().setComponent(new ComponentName(parent.getPackageName(),"org.example.Missing"))),"no matching parent returns false");
        settle(loop,parent::isResumed);check(nested.isDestroyed()&&parent.isResumed(),"missing-parent Up still finishes its caller toward the previous Activity");
        check(!parent.shouldUpRecreateTask(destination),"same task affinity retains the task");
        var affinity=ActivityClient.class.getDeclaredMethod("shouldUpRecreateTask",IBinder.class,String.class);affinity.setAccessible(true);
        check((boolean)affinity.invoke(ActivityClient.getInstance(),parent.getActivityToken(),"org.example.other"),"different destination affinity recreates the task");
        check(!(boolean)affinity.invoke(ActivityClient.getInstance(),nested.getActivityToken(),"org.example.other"),"a retired Activity cannot recreate another task");
        System.out.println("PASS original Up navigation: missing parent, task affinity and stale query");

        nested=child(parent,loop);
        Intent standard=new Intent(parent,HostWindowProbe.OwnedActivity.class).setAction("org.example.STANDARD").putExtra("fresh",73);
        check(nested.navigateUpTo(standard),"standard-parent Up is accepted");settle(loop,()->HostWindowProbe.OwnedActivity.instance!=parent&&HostWindowProbe.OwnedActivity.instance.isResumed());
        var replacement=HostWindowProbe.OwnedActivity.instance;
        check(parent.isDestroyed()&&nested.isDestroyed()&&replacement!=parent&&!replacement.isDestroyed(),"normal launch replaces the original standard parent");
        check(replacement.getIntent().filterEquals(standard)&&replacement.getIntent().getIntExtra("fresh",0)==73&&replacement.getTaskId()==task,"standard launch preserves the exact target Intent and task identity");
        replacement.getSystemService(ActivityManager.class).getAppTasks().getFirst().finishAndRemoveTask();HostWindowProbe.pump(loop);
        check(replacement.isDestroyed(),"replacement task retires through the original AppTask owner");
        System.out.println("PASS original Up navigation: standard parent replacement and original task retirement");
    }
}
