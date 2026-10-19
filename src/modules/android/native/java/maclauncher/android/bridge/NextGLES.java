package maclauncher.android.bridge;
/** Additional GLES entry points; all calls use the host's real ANGLE context. */
public final class NextGLES {
    static {System.load(java.util.Objects.requireNonNull(System.getProperty("klepton.graphics"),"klepton.graphics"));configure(System.getProperty("klepton.graphics"));}
    private static native void configure(String graphicsLibrary);
    public static void ensureLoaded(){}
    private static native void nUniform1iv(int location,int count,java.nio.IntBuffer values);
    public static native void activeTexture(int texture);
    public static native void enableDiagnostics();
    public static void uniform1iv(int location,int count,java.nio.IntBuffer values){
        java.util.Objects.requireNonNull(values,"uniform values");
        if(count<0||count>values.remaining())throw new IllegalArgumentException("GLES uniform count exceeds the buffer range");
        if(!values.isDirect()||!values.order().equals(java.nio.ByteOrder.nativeOrder()))throw new IllegalArgumentException("GLES uniforms require a native-order direct IntBuffer");
        nUniform1iv(location,count,values);
    }
}
