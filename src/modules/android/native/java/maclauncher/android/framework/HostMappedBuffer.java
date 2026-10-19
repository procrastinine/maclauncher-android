package maclauncher.android.framework;

import java.io.FileDescriptor;
import java.nio.MappedByteBuffer;

/** Actual JVM mapped buffers retain their descriptor and supplied cleanup owner. */
public final class HostMappedBuffer {
 private HostMappedBuffer(){}
 public static native MappedByteBuffer create(int capacity,long address,FileDescriptor descriptor,Runnable unmapper,boolean readOnly);
}
