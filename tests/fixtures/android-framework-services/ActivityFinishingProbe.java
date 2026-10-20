package maclauncher.android;
import android.app.Activity;
import android.content.Intent;
import android.os.Looper;
import java.util.List;
import maclauncher.android.framework.HostActivity;

/** Observe original callbacks through native initial and Android navigation owners. */
public final class ActivityFinishingProbe {
 private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
 public static void run(Activity activity,Looper loop)throws Exception {
  var initial=(HostWindowProbe.OwnedActivity)activity;
  check(!initial.isFinishing(),"initial Activity is running");
  initial.startActivity(new Intent(initial,HostWindowProbe.NextActivity.class));HostWindowProbe.pump(loop);
  var next=HostWindowProbe.NextActivity.instance;
  check(next!=null&&!next.isFinishing(),"original navigated Activity is running");
  check(initial.finishingCallbacks.contains("pause:false"),"ordinary navigation pauses without finishing");
  next.finish();HostWindowProbe.pump(loop);
  var expected=List.of("pause:true","stop:true","destroy:true");
  check(next.finishingCallbacks.equals(expected),"navigated callback finishing order: "+next.finishingCallbacks);
  check(!initial.isFinishing()&&!initial.isDestroyed(),"return keeps the initial Activity alive");
  initial.finishingCallbacks.clear();HostActivity.owner(initial).finish();HostWindowProbe.pump(loop);
  check(initial.finishingCallbacks.equals(expected),"initial callback finishing order: "+initial.finishingCallbacks);
  check(initial.isFinishing()&&initial.isDestroyed(),"initial original Activity reaches finished/destroyed state");
  System.out.println("PASS original initial and navigated Activity callbacks observe finishing before pause, stop and destroy");
 }
}
