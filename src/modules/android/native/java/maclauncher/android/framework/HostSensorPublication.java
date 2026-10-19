package maclauncher.android.framework;

import android.hardware.Sensor;
import android.os.SystemClock;
import java.io.*;
import java.lang.invoke.*;
import java.nio.*;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.locks.LockSupport;

/** Bounded physical sample transport for the original native sensor HAL.
 * HostSensors remains the sole descriptor, control and motion owner. */
public final class HostSensorPublication implements AutoCloseable {
    private static final VarHandle LONGS=MethodHandles.byteBufferViewVarHandle(long[].class,ByteOrder.LITTLE_ENDIAN);
    private final List<Sensor> sensors;
    private final FileChannel channel;
    private final MappedByteBuffer memory;
    private final FloatBuffer values;
    private final AtomicBoolean closed=new AtomicBoolean();
    private final Thread writer;
    private long sequence;

    public static HostSensorPublication open(Path directory){
        try{return new HostSensorPublication(directory);}
        catch(IOException failure){throw new UncheckedIOException(failure);}
    }
    private HostSensorPublication(Path directory)throws IOException{
        sensors=HostSensors.retainPublication();FileChannel opened=null;
        try{
            Files.createDirectories(directory);Files.setPosixFilePermissions(directory,java.nio.file.attribute.PosixFilePermissions.fromString("rwxr-xr-x"));
            int bytes=Math.addExact(16,Math.multiplyExact(sensors.size(),64));
            opened=FileChannel.open(directory.resolve("values"),StandardOpenOption.READ,StandardOpenOption.WRITE,StandardOpenOption.CREATE);
            Files.setPosixFilePermissions(directory.resolve("values"),java.nio.file.attribute.PosixFilePermissions.fromString("rw-r--r--"));
            opened.truncate(0);opened.position(bytes-1);opened.write(ByteBuffer.wrap(new byte[1]));
            memory=opened.map(FileChannel.MapMode.READ_WRITE,0,bytes);memory.order(ByteOrder.LITTLE_ENDIAN);
            ByteBuffer payload=memory.duplicate();payload.position(16);
            values=payload.slice().order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer();
            publish();
            Path temporary=directory.resolve("descriptors.tmp");
            try(var out=new DataOutputStream(Files.newOutputStream(temporary))){
                out.writeLong(0x4d4c53454e533031L);out.writeInt(sensors.size());
                for(Sensor sensor:sensors){
                    out.writeInt(sensor.getHandle());out.writeInt(sensor.getType());out.writeInt(sensor.getVersion());
                    out.writeInt(sensor.getMinDelay());out.writeInt(sensor.getMaxDelay());out.writeInt(sensor.getReportingMode()<<1);
                    out.writeFloat(sensor.getMaximumRange());out.writeFloat(sensor.getResolution());out.writeFloat(sensor.getPower());
                    out.writeInt(HostSensors.publicationEncoding(sensor.getType()));
                    for(String text:List.of(sensor.getName(),sensor.getVendor(),sensor.getStringType(),sensor.getRequiredPermission())){
                        byte[] utf8=text.getBytes(java.nio.charset.StandardCharsets.UTF_8);out.writeInt(utf8.length);out.write(utf8);
                    }
                }
            }
            Files.setPosixFilePermissions(temporary,java.nio.file.attribute.PosixFilePermissions.fromString("rw-r--r--"));
            Files.move(temporary,directory.resolve("descriptors"),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
            channel=opened;
            writer=sensors.isEmpty()?null:new Thread(this::run,"Android sensor publication");
            if(writer!=null){writer.setDaemon(true);writer.start();}
        }catch(Throwable failure){if(opened!=null)try{opened.close();}catch(Throwable close){failure.addSuppressed(close);}HostSensors.releasePublication();throw failure;}
    }
    private void publish(){
        LONGS.setVolatile(memory,0,++sequence);
        long now=SystemClock.elapsedRealtimeNanos();values.clear();HostSensors.copyPublication(now,sensors,values);
        memory.putLong(8,now);LONGS.setRelease(memory,0,++sequence);
    }
    private void run(){
        long next=System.nanoTime();while(!closed.get()){
            publish();next+=16_667_000L;long now=System.nanoTime();if(next<now)next=now;
            LockSupport.parkNanos(Math.max(1,next-now));
        }
    }
    @Override public void close(){
        if(!closed.compareAndSet(false,true))return;
        if(writer!=null){writer.interrupt();boolean interrupted=false;for(;;)try{writer.join();break;}catch(InterruptedException again){interrupted=true;}if(interrupted)Thread.currentThread().interrupt();}
        try{channel.close();}catch(IOException failure){throw new UncheckedIOException(failure);}finally{HostSensors.releasePublication();}
    }
}
