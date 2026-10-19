package maclauncher.android.framework;

import java.io.Closeable;
import java.io.FileDescriptor;
import java.nio.ByteBuffer;
import java.nio.Buffer;
import java.nio.channels.FileChannel;

/** Android's NIO helper contract uses the JVM's buffer, descriptor and cleaner owners. */
public final class HostNio {
    private HostNio() {}
    public static byte[] unsafeArray(ByteBuffer buffer) { return buffer.array(); }
    public static int unsafeArrayOffset(ByteBuffer buffer) { return buffer.arrayOffset(); }
    public static boolean isDirectByteBuffer(Object buffer) { return buffer instanceof ByteBuffer value && value.isDirect(); }
    /** JVM callers only: ART retains its original ByteBuffer MemoryRef. */
    public static void track(Buffer buffer) { if(buffer!=null&&buffer.isDirect()) Access.call("track",new Class<?>[]{Buffer.class},buffer); }
    public static void trackBuffers(ByteBuffer[] buffers) { if(buffers!=null)for(ByteBuffer buffer:buffers)track(buffer); }
    public static void setAccessible(ByteBuffer buffer,boolean accessible) { Access.call("setAccessible",new Class<?>[]{ByteBuffer.class,boolean.class},buffer,accessible); }
    public static boolean isAccessible(ByteBuffer buffer) { return (boolean)Access.call("isAccessible",new Class<?>[]{ByteBuffer.class},buffer); }
    private static final class Access {
        private static final Class<?> OWNER;
        static {
            try { OWNER=Class.forName("maclauncher.android.framework.JvmNioAccess"); }
            catch(ClassNotFoundException failure){throw new ExceptionInInitializerError(failure);}
        }
        static Object call(String name,Class<?>[] parameters,Object...arguments){
            try{return OWNER.getMethod(name,parameters).invoke(null,arguments);}
            catch(java.lang.reflect.InvocationTargetException failure){
                Throwable cause=failure.getCause();if(cause instanceof RuntimeException error)throw error;if(cause instanceof Error error)throw error;
                throw new IllegalStateException("JVM direct-buffer access failed",cause);
            }catch(ReflectiveOperationException failure){throw new IllegalStateException("JVM direct-buffer access is unavailable",failure);}
        }
    }
    public static native void freeDirectBuffer(ByteBuffer buffer);
    public static native FileDescriptor getFD(FileChannel channel);
    public static FileChannel newFileChannel(Closeable parent,FileDescriptor descriptor,int mode) {
        return openChannel(parent,descriptor,(mode&3)!=1,(mode&3)!=0);
    }
    private static native FileChannel openChannel(Closeable parent,FileDescriptor descriptor,boolean readable,boolean writable);
}
