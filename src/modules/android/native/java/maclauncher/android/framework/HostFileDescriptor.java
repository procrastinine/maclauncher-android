package maclauncher.android.framework;
import java.io.FileDescriptor;
import java.util.Objects;
import java.util.WeakHashMap;
/** ART descriptor operations over the actual JDK descriptor and ownership tag. */
public final class HostFileDescriptor {
    private HostFileDescriptor(){}
    private static final WeakHashMap<FileDescriptor,Long> owners=new WeakHashMap<>();
    private static final WeakHashMap<FileDescriptor,Long> nativeOwners=new WeakHashMap<>();
    public static native int getInt(FileDescriptor descriptor);
    public static native void setInt(FileDescriptor descriptor,int value);
    public static native boolean isSocket(FileDescriptor descriptor);
    private static native int detach(FileDescriptor descriptor);
    private static native void registerCleanup(FileDescriptor descriptor);
    /** A raw Parcel descriptor owns its duplicate even before a stream is attached. */
    public static FileDescriptor duplicate(FileDescriptor descriptor)throws android.system.ErrnoException{
        FileDescriptor copy=android.system.Os.dup(descriptor);
        try{registerCleanup(copy);return copy;}catch(RuntimeException|Error failure){try{android.system.Os.close(copy);}catch(android.system.ErrnoException close){failure.addSuppressed(close);}throw failure;}
    }
    private static native FileDescriptor socketDescriptor(Object socket,boolean datagram)throws java.io.IOException;
    public static FileDescriptor socketDescriptor(java.net.Socket socket)throws java.io.IOException{return socketDescriptor(Objects.requireNonNull(socket),false);}
    public static FileDescriptor socketDescriptor(java.net.DatagramSocket socket)throws java.io.IOException{return socketDescriptor(Objects.requireNonNull(socket),true);}
    public static void android_fdsan_exchange_owner_tag(FileDescriptor descriptor,long expected,long value){Objects.requireNonNull(descriptor);synchronized(owners){long actual=nativeOwners.getOrDefault(descriptor,0L);if(actual!=expected)throw new IllegalStateException("File descriptor owner changed");if(value==0)nativeOwners.remove(descriptor);else nativeOwners.put(descriptor,value);}}
    public static long android_fdsan_get_owner_tag(FileDescriptor descriptor){Objects.requireNonNull(descriptor);synchronized(owners){return nativeOwners.getOrDefault(descriptor,0L);}}
    public static long android_fdsan_get_tag_value(long tag){return tag&0x00ffffffffffffffL;}
    public static String android_fdsan_get_tag_type(long tag){return switch((int)(tag>>>56)){case 0->"unowned";case 5->"FileInputStream";case 6->"FileOutputStream";case 7->"RandomAccessFile";case 8->"ParcelFileDescriptor";case 10->"DatagramSocketImpl";case 11->"SocketImpl";default->"generic";};}
    public static long getOwnerId(FileDescriptor descriptor){Objects.requireNonNull(descriptor);synchronized(owners){return owners.getOrDefault(descriptor,0L);}}
    public static void setOwnerId(FileDescriptor descriptor,long value){Objects.requireNonNull(descriptor);synchronized(owners){if(value==0)owners.remove(descriptor);else owners.put(descriptor,value);}}
    public static FileDescriptor release(FileDescriptor descriptor){
        Objects.requireNonNull(descriptor);synchronized(descriptor){
            FileDescriptor result=new FileDescriptor();setInt(result,detach(descriptor));
            synchronized(owners){Long owner=owners.remove(descriptor);if(owner!=null)owners.put(result,owner);Long nativeOwner=nativeOwners.remove(descriptor);if(nativeOwner!=null)nativeOwners.put(result,nativeOwner);}
            return result;
        }
    }
    public static void cloneForFork(FileDescriptor descriptor){Objects.requireNonNull(descriptor);throw new UnsupportedOperationException("Forking the Java runtime is unavailable");}
}
