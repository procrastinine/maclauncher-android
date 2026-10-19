package maclauncher.android.framework;

import android.os.*;
import android.os.storage.*;
import java.io.IOException;
import java.nio.file.*;
import java.util.Objects;

/** The private application's actual mount endpoint beneath AOSP StorageManager. */
public final class HostStorageService extends IStorageManager.Default {
    private static final HostStorageService INSTANCE=new HostStorageService();
    private final RemoteCallbackList<IStorageEventListener> listeners=new RemoteCallbackList<>();
    private static boolean installed;
    private HostStorageService(){}
    public static synchronized void install(){
        if(installed)return;
        HostServices.register("mount",INSTANCE,"android.os.storage.IStorageManager");installed=true;
    }
    private static void requirePackage(String name){if(!Objects.equals(name,HostFilesystem.packageName()))throw new SecurityException("Storage belongs to another application");}
    private static int state(Path root){return !Files.isDirectory(root)||!Files.isReadable(root)?VolumeInfo.STATE_UNMOUNTED:Files.isWritable(root)?VolumeInfo.STATE_MOUNTED:VolumeInfo.STATE_MOUNTED_READ_ONLY;}
    @Override public StorageVolume[] getVolumeList(int user,String packageName,int flags){
        requirePackage(packageName);if(user!=UserHandle.myUserId())return new StorageVolume[0];
        Path root=HostFilesystem.storageRoot();
        return new StorageVolume[]{new StorageVolume(VolumeInfo.ID_EMULATED_INTERNAL,root.toFile(),root.toFile(),"Android application storage",true,false,true,false,false,0,new UserHandle(user),null,null,VolumeInfo.getEnvironmentForState(state(root)))};
    }
    @Override public VolumeInfo[] getVolumes(int flags){
        var volume=new VolumeInfo(VolumeInfo.ID_PRIVATE_INTERNAL,VolumeInfo.TYPE_PRIVATE,null,null);
        volume.path=HostFilesystem.dataRoot().toString();volume.internalPath=volume.path;
        volume.mountUserId=UserHandle.myUserId();volume.state=state(HostFilesystem.dataRoot());
        return new VolumeInfo[]{volume};
    }
    @Override public DiskInfo[] getDisks(){return new DiskInfo[0];}
    @Override public VolumeRecord[] getVolumeRecords(int flags){return new VolumeRecord[0];}
    @Override public synchronized void registerListener(IStorageEventListener listener){
        listeners.register(Objects.requireNonNull(listener));
    }
    @Override public synchronized void unregisterListener(IStorageEventListener listener){if(listener!=null)listeners.unregister(listener);}
    @Override public void mkdirs(String packageName,String path){
        requirePackage(packageName);
        Path requested=Path.of(HostFilesystem.mapPath(path)).toAbsolutePath().normalize(),existing=requested;
        Path root=HostFilesystem.dataRoot();
        if(!requested.startsWith(root))throw new SecurityException("Storage path is outside the application directory");
        try{
            while(existing!=null&&!Files.exists(existing,LinkOption.NOFOLLOW_LINKS))existing=existing.getParent();
            if(existing==null||!existing.toRealPath().startsWith(root))throw new SecurityException("Storage path leaves the application directory");
            Files.createDirectories(requested);
        }catch(IOException error){throw new IllegalStateException("Application storage directory could not be created",error);}
    }
}
