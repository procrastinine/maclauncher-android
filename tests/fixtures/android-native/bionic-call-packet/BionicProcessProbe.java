public final class BionicProcessProbe {
 private static native void prepare();
 private static native long roundTrip(int a,long b,float c,double d,int e,int f,int g,int h,int i,int j);
 private static native long nested(long value);
 private static native long narrow(boolean z,byte b,char c,short s,int i,long l,int a,int d,int e,int f);
 private static native double floating(float a0,float a1,float a2,float a3,float a4,float a5,float a6,float a7,float a8,double d0,double d1,double d2,double d3,double d4,double d5,double d6,double d7,double d8);
 private static native Object object(Object value);
 private static native boolean booleanValue(boolean v);private static native byte byteValue(byte v);private static native char charValue(char v);private static native short shortValue(short v);private static native int intValue(int v);private static native long longValue(long v);private static native float floatValue(float v);private static native void voidValue(int v);private static native void throwing();
 private static native void complete();
 private static final RuntimeException EXPECTED=new RuntimeException("fixture original");
 public static long callback(long value){return nested(value)+7;}
 public static void throwCallback(){throw EXPECTED;}
 public static void main(String[] args)throws Exception{
  Throwable[] failure={null};Thread worker=new Thread(()->{try{prepare();for(int n=0;n<2;n++){
   if(roundTrip(11,22,1.5f,2.5,33,44,55,66,77,88)!=411)throw new AssertionError("mixed/nested");
   if(narrow(true,(byte)-7,(char)65530,(short)-30000,0x12345678,0x1122334455667788L,-11,22,-33,44)!=0x8877665544332211L)throw new AssertionError("narrow spills");
   if(floating(.25f,1.25f,2.25f,3.25f,4.25f,5.25f,6.25f,7.25f,8.25f,1000.5,1001.5,1002.5,1003.5,1004.5,1005.5,1006.5,1007.5,1008.5)!=1016.75)throw new AssertionError("FP spills");
   Object value=new Object();if(object(value)!=value||booleanValue(true)||byteValue((byte)-100)!=-101||charValue((char)60000)!=60001||shortValue((short)-30000)!=-29999||intValue(0x7abc1234)!=(0x7abc1234^0x11223344)||longValue(0x76543210abcdef12L)!=(0x76543210abcdef12L^0x1122334455667788L)||floatValue(1.25f)!=2.5f)throw new AssertionError("return/reference");voidValue(73);
  }try{throwing();throw new AssertionError("missing original Throwable");}catch(RuntimeException e){if(e!=EXPECTED)throw new AssertionError("Throwable changed",e);}
  }catch(Throwable error){failure[0]=error;}},"PacketJavaThread");worker.start();worker.join();if(failure[0]!=null)throw new AssertionError(failure[0]);complete();System.out.println("PASS ordinary Darwin Java generic AAPCS64 packet real ELF descriptor spills vector and integer returns nested JNI callback Throwable identity and same-thread retirement");
 }
}
