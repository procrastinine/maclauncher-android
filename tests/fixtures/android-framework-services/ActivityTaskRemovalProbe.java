package maclauncher.android;
import android.app.*;
import android.content.Intent;
import android.os.Looper;
import java.lang.reflect.*;
import maclauncher.android.framework.*;
import org.json.JSONObject;

/** Original nested tasks and finish modes through the existing physical window fixture. */
public final class ActivityTaskRemovalProbe {
 private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
 private static HostWindowProbe.NextActivity child(Activity root,Looper loop)throws Exception{
  root.startActivity(new Intent(root,HostWindowProbe.NextActivity.class));HostWindowProbe.pump(loop);
  var child=HostWindowProbe.NextActivity.instance;check(child!=null&&!child.isDestroyed(),"original child is running");return child;
 }
 private static HostWindowProbe.OwnedActivity root(Activity previous,Looper loop)throws Exception{
  HostWindowManager.detachOutput();
  var next=new HostWindowProbe.OwnedActivity();HostActivity.attach(next,previous.getBaseContext(),previous.getApplication());HostActivity.start(next,null);HostActivity.resume(next);
  HostWindowManager.attachOutput(next,HostWindowProbe.fixtureOutput,320,240);HostWindowProbe.pump(loop);return next;
 }
 private static void close()throws Exception{
  var command=Class.forName("maclauncher.test.AndroidTestControl").getDeclaredMethod("command",JSONObject.class);command.setAccessible(true);
  try{command.invoke(null,new JSONObject().put("op","close"));}catch(InvocationTargetException failure){if(failure.getCause()instanceof Exception exception)throw exception;throw failure;}
 }
 public static void run(Activity initial,Looper loop)throws Exception{
  var initialOwner=HostActivity.owner(initial);var nested=child(initial,loop);maclauncher.test.AndroidTestControl.attached(nested);
  close();HostWindowProbe.pump(loop);
  check(initial.isDestroyed()&&nested.isDestroyed()&&initialOwner.closing(),"test close removes both original nested Activities");
  System.out.println("PASS targeted test close removes the original nested task without signal fallback");
  var root=root(initial,loop);
  try{close();throw new AssertionError("stale Activity closed a replacement task");}catch(IllegalStateException expected){}
  check(!root.isDestroyed(),"stale close leaves the replacement task alive");
  System.out.println("PASS stale test close cannot fall through to a different task");
  var nonroot=child(root,loop);nonroot.finishAndRemoveTask();HostWindowProbe.pump(loop);
  check(nonroot.isDestroyed()&&!root.isDestroyed()&&!HostActivity.owner(root).closing(),"original non-root mode1 finishes only its Activity");
  System.out.println("PASS original non-root finishAndRemoveTask retains its root");
  var rootOwner=HostActivity.owner(root);var descendant=child(root,loop);root.finishAndRemoveTask();HostWindowProbe.pump(loop);
  check(root.isDestroyed()&&descendant.isDestroyed()&&rootOwner.closing(),"original root mode1 removes the entire nested task");
  System.out.println("PASS original root finishAndRemoveTask removes its descendants");
  var lastRoot=root(root,loop);var lastOwner=HostActivity.owner(lastRoot);var lastChild=child(lastRoot,loop);
  check(ActivityClient.getInstance().finishActivity(lastChild.getActivityToken(),Activity.RESULT_CANCELED,null,Activity.FINISH_TASK_WITH_ACTIVITY),"original always-remove request accepted");
  HostWindowProbe.pump(loop);check(lastRoot.isDestroyed()&&lastChild.isDestroyed()&&lastOwner.closing(),"original mode2 removes a task from its non-root Activity");
  System.out.println("PASS original always-remove mode removes the task from a non-root Activity");
 }
}
