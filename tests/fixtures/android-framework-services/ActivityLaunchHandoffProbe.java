package maclauncher.android;
import android.app.Activity;
import android.content.Intent;
import android.os.Looper;
import java.lang.reflect.Method;
import maclauncher.android.framework.*;

/** Original task requests remain owned across a physical display turn. */
public final class ActivityLaunchHandoffProbe {
 private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
 public static void run(Activity initial,Looper loop)throws Exception{
  var owner=HostActivity.owner(initial);
  Method interval=HostLooper.class.getDeclaredMethod("hostFrameInterval",long.class);interval.setAccessible(true);
  // One completed Android callback exhausts the display turn. No wall-clock
  // sleep or arbitrary presentation grace period is involved.
  interval.invoke(null,1L);
  try{
   initial.finish();
   initial.startActivity(new Intent(initial,HostWindowProbe.NextActivity.class));
   for(int count=0;!initial.isDestroyed()&&count<1024;count++)owner.pump();
   check(initial.isDestroyed(),"original finishing callback completed");
   check(HostWindowProbe.NextActivity.instance==null,"successor is queued beyond the current display turn");
   check(!owner.closing(),"accepted local successor keeps the native task alive across the empty turn");
   for(int count=0;HostWindowProbe.NextActivity.instance==null&&count<1024;count++)owner.pump();
   var next=HostWindowProbe.NextActivity.instance;
   check(next!=null&&!next.isDestroyed()&&!owner.closing(),"successor becomes the live original Activity");
   interval.invoke(null,16_666_667L);
   next.finish();
   // The new original ViewRoot owns a traversal barrier. Deliver its display
   // frame before expecting the synchronous finish message to pass it.
   for(int count=0;!next.isDestroyed()&&count<16;count++)HostWindowProbe.pump(loop);
   check(next.isDestroyed()&&owner.closing(),"the final accepted task retires without a grace delay");
   System.out.println("PASS original finish-before-start handoff: queued successor survives a display-turn boundary and final task retires");
  }finally{interval.invoke(null,16_666_667L);}
 }
}
