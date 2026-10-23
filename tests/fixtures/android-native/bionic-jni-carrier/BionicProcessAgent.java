import java.lang.instrument.Instrumentation;
public final class BionicProcessAgent {
 private static native int initialize(String[] paths);
 public static void premain(String ignored,Instrumentation instrumentation){
  String bridge=System.getProperty("bionic.bridge");System.load(bridge);
  if(initialize(new String[]{System.getProperty("bionic.abi"),System.getProperty("bionic.root"),System.getProperty("bionic.map"),"/data/local/tmp/bionic-admission/probe.so",System.getProperty("bionic.runtime"),bridge,"uid\t10000\ngid\t10000\ncap_effective\t0\ncap_permitted\t0\ncap_bounding\t0\nseclabel\tu:r:untrusted_app:s0\n"})!=0)throw new IllegalStateException("Bionic process initialization");
 }
}
