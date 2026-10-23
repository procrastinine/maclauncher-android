public final class BionicAdmissionProbe {
 private static native long roundTrip(int a,long b,float c,double d,int e,int f,int g,int h,int i,int j);
 private static native long nested(long value);
 private static Throwable failure;
 public static long callback(long value) { return nested(value)+7; }
 public static void run() throws Exception {
  Thread worker=new Thread(()->{try{if(roundTrip(11,22,1.5f,2.5,33,44,55,66,77,88)!=411)throw new AssertionError("native result");}catch(Throwable error){failure=error;}},"BionicAdmissionJavaThread");
  worker.start();worker.join();if(failure!=null)throw new AssertionError(failure);
 }
}
