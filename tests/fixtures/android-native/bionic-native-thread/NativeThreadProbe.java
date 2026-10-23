package maclauncher.android.bridge;
import java.lang.instrument.Instrumentation;
import java.nio.file.Path;

public final class NativeThreadProbe {
 private static native int initialize(String[] options,int groups);
 private static native void call(String core,String file,java.util.function.LongConsumer preparation);
 private static native void complete();

 public static void premain(String ignored,Instrumentation instrumentation) {
  System.load(System.getProperty("consumer.core"));
  System.load(System.getProperty("consumer.binding"));
  System.load(System.getProperty("looper.helper"));
  maclauncher.android.framework.HostProcessIdentity.initializeIds();

 }

 public static void main(String[] args) throws Exception {
  String core=System.getProperty("consumer.core");
  String file=Path.of(args[0],"libnative-thread-client.so").toString();
  Path data=Path.of(System.getProperty("bionic.runtime")).getParent();
  var bootstrap=Class.forName("maclauncher.android.RuntimeBootstrap").getDeclaredMethod("initialize",Path.class,Path.class,Path.class,Path.class,String.class,boolean.class,boolean.class,boolean.class);
  bootstrap.setAccessible(true);
  bootstrap.invoke(null,Path.of(System.getProperty("ndk.runtime")),data,data.resolve("files"),data.resolve("cache"),"owned.native.thread",false,false,false);
  GuestNativeLoader.configure(core,args[0],args[1],NativeThreadProbe.class.getClassLoader());
  int groups=Integer.getInteger("native.thread.groups",7);
  int status=initialize(new String[]{
   System.getProperty("bionic.abi"),System.getProperty("bionic.root"),
   System.getProperty("bionic.map"),"/data/local/tmp/bionic-runtime/probe.so",
   System.getProperty("bionic.runtime"),System.getProperty("consumer.binding"),
   "uid\t10000\ngid\t10000\ncap_effective\t0\ncap_permitted\t0\ncap_bounding\t0\nseclabel\tu:r:untrusted_app:s0\n"
  },groups);
  if(status!=0)throw new IllegalStateException("Native thread control initialization: "+status);
  // The guest library creates and joins its own Bionic pthread. This Java
  // Thread only owns image preparation and retires its admitted carrier.
  Throwable[] failure={null};
  Thread owner=new Thread(()->{
   try { call(core,file,image->{if(image==0)throw new AssertionError("Missing prepared image");}); }
   catch(Throwable error) { failure[0]=error; }
  },"NativeThreadImageOwner");
  owner.start();owner.join();
  if(failure[0]!=null)throw new AssertionError(failure[0]);
  complete();
  System.out.println("PASS original Bionic pthread pure-C looper callback same physical thread; GetEnv JNI_EDETACHED before inside after; NDK graphics EGL CPU semantics; image owner and descriptor retirement; no game/window/GPU");
 }
}
