package maclauncher.android.framework;
/** Initializes the complete upstream JNI adapters on the existing ANGLE provider. */
public final class HostGles {
 private HostGles(){}
 static {System.load(System.getProperty("klepton.graphics"));}
 public static synchronized native void register(Class<?> type);
}
