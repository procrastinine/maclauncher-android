package maclauncher.android.bridge;
public final class Graphics {
    static {System.load(System.getProperty("klepton.graphics"));}
    public static native void configureHost(String graphicsLibrary,String angleDirectory,int width,int height);
    public static native int vulkanVersion();
    public static native int vulkanHardwareLevel();
}
