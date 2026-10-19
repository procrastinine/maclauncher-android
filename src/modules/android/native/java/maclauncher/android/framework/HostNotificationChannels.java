package maclauncher.android.framework;
import android.app.NotificationChannel;

import android.content.Context;
import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Durable channel configuration. No host notification permission or delivery. */
public final class HostNotificationChannels {
    private static final Object LOCK=new Object();
    private static final int LEGACY_MAGIC=0x4d4c4e31,MAGIC=0x4d4c4e32,MAX_CHANNELS=256,MAX_BYTES=1048576;
    private final Path file;
    public HostNotificationChannels(Context context){this(context.getFilesDir().toPath().resolve(".maclauncher-platform/notification-channels.bin"));}
    public HostNotificationChannels(Path file){this.file=file.toAbsolutePath().normalize();}
    public void createNotificationChannel(NotificationChannel channel){createNotificationChannels(List.of(channel));}
    public void createNotificationChannels(List<NotificationChannel> channels){synchronized(LOCK){
        Objects.requireNonNull(channels);if(channels.size()>MAX_CHANNELS)throw new IllegalArgumentException("Too many notification channels");
        var current=read();
        for(var value:channels){Objects.requireNonNull(value);var previous=current.get(value.getId());
            if(previous==null){if(current.size()>=MAX_CHANNELS)throw new IllegalStateException("Notification channel limit reached");current.put(value.getId(),value.copy());}
            else if(previous.isDeleted()){previous.setDeleted(false);}
            else {previous.setName(value.getName());previous.setDescription(value.getDescription());previous.setImportance(Math.min(previous.getImportance(),value.getImportance()));}
        }
        write(current);
    }}
    public NotificationChannel getNotificationChannel(String id){synchronized(LOCK){var channel=read().get(Objects.requireNonNull(id));return channel==null||channel.isDeleted()?null:channel.copy();}}
    public List<NotificationChannel> getNotificationChannels(){synchronized(LOCK){var result=new ArrayList<NotificationChannel>();for(var channel:read().values())if(!channel.isDeleted())result.add(channel.copy());return result;}}
    public void deleteNotificationChannel(String id){synchronized(LOCK){var current=read();var channel=current.get(Objects.requireNonNull(id));if(channel!=null&&!channel.isDeleted()){channel.setDeleted(true);write(current);}}}
    private LinkedHashMap<String,NotificationChannel> read(){
        var result=new LinkedHashMap<String,NotificationChannel>();
        if(!Files.exists(file,LinkOption.NOFOLLOW_LINKS))return result;
        try{
            if(!Files.isRegularFile(file,LinkOption.NOFOLLOW_LINKS))throw new IOException("Notification channel store is not a regular file");
            byte[] bytes;try(var input=Files.newInputStream(file)){bytes=input.readNBytes(MAX_BYTES+1);}if(bytes.length>MAX_BYTES)throw new IOException("Notification channel store is too large");
            try(var input=new DataInputStream(new ByteArrayInputStream(bytes))){
                int format=input.readInt();if(format!=MAGIC&&format!=LEGACY_MAGIC)throw new IOException("Invalid notification channel store");int count=input.readInt();if(count<0||count>MAX_CHANNELS)throw new IOException("Invalid notification channel count");
                for(int i=0;i<count;i++){
                    NotificationChannel channel;
                    if(format==LEGACY_MAGIC){
                        channel=new NotificationChannel(input.readUTF(),input.readUTF(),input.readInt());
                        if(input.readBoolean())channel.setDescription(input.readUTF());channel.enableLights(input.readBoolean());channel.setLightColor(input.readInt());channel.setShowBadge(input.readBoolean());channel.setLockscreenVisibility(input.readInt());channel.setDeleted(input.readBoolean());
                    }else{
                        int length=input.readInt();if(length<0||length>MAX_BYTES||length>input.available())throw new IOException("Invalid channel record length");
                        byte[] encoded=input.readNBytes(length);android.os.Parcel parcel=android.os.Parcel.obtain();
                        try{parcel.unmarshall(encoded,0,encoded.length);parcel.setDataPosition(0);channel=NotificationChannel.CREATOR.createFromParcel(parcel);if(parcel.dataAvail()!=0)throw new IOException("Trailing channel record");}finally{parcel.recycle();}
                    }
                    if(result.put(channel.getId(),channel)!=null)throw new IOException("Duplicate notification channel");
                }
                if(input.available()!=0)throw new IOException("Trailing notification channel data");
            }
            return result;
        }catch(IOException error){throw new UncheckedIOException(error);}
    }
    private void write(Map<String,NotificationChannel> channels){
        Path temporary=null;
        try{
            var bytes=new ByteArrayOutputStream();try(var output=new DataOutputStream(bytes)){output.writeInt(MAGIC);output.writeInt(channels.size());
                for(var channel:channels.values()){
                    android.os.Parcel parcel=android.os.Parcel.obtain();
                    try{channel.writeToParcel(parcel,0);if(parcel.dataSize()>MAX_BYTES)throw new IOException("Notification channel is too large");byte[] encoded=parcel.marshall();if(encoded.length>MAX_BYTES-bytes.size()-4)throw new IOException("Notification channel is too large");output.writeInt(encoded.length);output.write(encoded);}finally{parcel.recycle();}
                }
            }
            if(bytes.size()>MAX_BYTES)throw new IOException("Notification channel store is too large");
            Files.createDirectories(file.getParent());if(Files.isSymbolicLink(file.getParent())||Files.isSymbolicLink(file))throw new IOException("Notification channel store must not be linked");
            temporary=Files.createTempFile(file.getParent(),"channels-",".tmp");Files.write(temporary,bytes.toByteArray());Files.move(temporary,file,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);temporary=null;
        }catch(IOException error){throw new UncheckedIOException(error);}
        finally{if(temporary!=null)try{Files.deleteIfExists(temporary);}catch(IOException ignored){}}
    }
}
