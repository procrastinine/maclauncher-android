import java.util.Arrays;import java.util.concurrent.*;import java.util.concurrent.atomic.*;

/** The GPU transport owns readback; public PixelCopy APIs are checked separately. */
public final class PixelCopyProbe {
 public interface Completion {void complete(int status,int[] pixels);}
 static native void copy(Completion callback,int mode);
 static native int sourceRetirements();
 static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
 public static void main(String[] args)throws Exception {
  System.load(args[0]);
  for(int mode:new int[]{0,5,6,7,1,2,3,8,9,10}){
   var done=new CountDownLatch(1);var status=new AtomicInteger(-1);var result=new AtomicReference<int[]>();
   copy((code,pixels)->{status.set(code);result.set(pixels);done.countDown();},mode);
   check(done.await(2,TimeUnit.SECONDS),"GPU completion deadline");check(status.get()==(mode==1?3:mode==2?4:mode==3?2:0),"GPU status "+mode);
   if(mode==0||mode==5||mode==8||mode==9||mode==10)check(Arrays.equals(result.get(),new int[]{0x400000ff,0xffffffff,0x80ff0000,0xff00ff00}),"RGBA/BGRA orientation and retained source "+mode);
   if(mode==6){int pixel=result.get()[0];check((pixel&0xffffff)==0x808080&&(pixel>>>24)==176,"linear scaling");}
   if(mode==7)check(result.get().length==1&&result.get()[0]==0x80ff0000,"selected source region");
  }
  long retiredDeadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(1);
  while(sourceRetirements()!=3&&System.nanoTime()<retiredDeadline)Thread.sleep(1);
  check(sourceRetirements()==3,"quiescent and replaced source leases retired after GPU reads");
  var cancelled=new AtomicInteger();copy((status,pixels)->cancelled.incrementAndGet(),4);Thread.sleep(30);check(cancelled.get()==0,"cancelled copy callback retired");
  System.out.println("PixelCopy GPU/regions/lifetime PASS; quiescent source and pooled reuse PASS");
 }
}
