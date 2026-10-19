package maclauncher.android.framework;

/** JNI array ownership for the unchanged AOSP modified-UTF codec. */
public final class HostCharset {
    private HostCharset() {}
    public static int toModifiedUtf8Bytes(String source,byte[] destination,int offset,int capacity){
        return encode(source,source.length(),destination,offset,capacity);
    }
    private static native int encode(String source,int length,byte[] destination,int offset,int capacity);
    public static native String fromModifiedUtf8Bytes(byte[] source,int offset,int length);
}
