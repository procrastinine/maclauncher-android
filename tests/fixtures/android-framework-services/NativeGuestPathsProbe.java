import maclauncher.android.framework.HostFilesystem;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public final class NativeGuestPathsProbe {
    static native byte[] read(byte[] path)throws java.io.IOException;
    static native byte[] resolve(byte[] path,int capacity);
    private static native void storageStats(byte[] guest,byte[] host);
    private static byte[] bytes(String text){return text.getBytes(StandardCharsets.UTF_8);}
    private static String mapped(String text){return new String(resolve(bytes(text),4096),StandardCharsets.UTF_8);}
    private static void equal(Object actual,Object expected){if(!Objects.equals(actual,expected))throw new AssertionError(actual+" != "+expected);}
    public static void main(String[] args)throws Exception{
        Path data=Path.of(args[0]).toRealPath(),files=Files.createDirectory(data.resolve("files")),cache=Files.createDirectory(data.resolve("cache")),assets=Files.createDirectory(data.resolve("assets"));
        Path base=Files.writeString(data.resolve("source.apk"),"original base ZIP"),split=Files.writeString(data.resolve("split-1.apk"),"original split ZIP");
        HostFilesystem.configure(data,data,files,cache,"owned.filesystem");
        if(args.length>2)System.load(args[2]);
        System.load(args[1]);
        HostFilesystem.installNativeMappings(List.of(base,split),assets);
        if(args.length>2){
            Path proc=Path.of(mapped("/proc"));
            if(!Files.isDirectory(proc))throw new AssertionError("Guest /proc parent directory is absent");
            Path meminfo=Path.of(mapped("/proc/meminfo"));
            equal(meminfo.getParent(),proc);
            String memory=new String(read(bytes("/proc/meminfo")),StandardCharsets.UTF_8);
            if(!memory.startsWith("MemTotal:")||!memory.contains("MemAvailable:"))throw new AssertionError("Guest memory report is absent");
            equal(mapped("/proc/not-an-implemented-file"),"/proc/not-an-implemented-file");
            equal(mapped("/proc-other"),"/proc-other");
        }
        equal(HostFilesystem.mapPath("/data"),data.toString());
        Path etc=Files.createDirectories(data.resolve("android-root/system/etc"));
        Files.writeString(etc.resolve("passwd"),"private Android account database");
        equal(mapped("/etc/passwd"),HostFilesystem.mapPath("/system/etc/passwd"));
        equal(new String(read(bytes("/etc/passwd")),StandardCharsets.UTF_8),"private Android account database");
        String asset="保存🍬.txt";Files.writeString(assets.resolve(asset),"actual extracted bytes");
        Files.writeString(files.resolve("external.txt"),"private storage");
        for(Path apk:List.of(base,split)){
            String virtual=apk+"/assets/"+asset;
            equal(mapped(virtual),HostFilesystem.mapPath(virtual));
            equal(new String(read(bytes(virtual)),StandardCharsets.UTF_8),"actual extracted bytes");
            equal(mapped(apk.toString()),apk.toString());
            equal(new String(read(bytes(apk.toString())),StandardCharsets.UTF_8),Files.readString(apk));
            equal(mapped(apk+"/assets-other/file"),apk+"/assets-other/file");
        }
        for(String path:List.of("/sdcard/external.txt","/mnt/sdcard/external.txt","/storage/emulated/0/external.txt","/storage/self/primary/external.txt","/data/user/0/owned.filesystem/files/external.txt")){
            equal(mapped(path),HostFilesystem.mapPath(path));equal(new String(read(bytes(path)),StandardCharsets.UTF_8),"private storage");
            storageStats(bytes(path),bytes(files.resolve("external.txt").toString()));
        }
        // Original Environment composes these scoped paths from the volume's
        // private-files root. A retained Android filename needs one native lookup.
        for(String prefix:List.of("/data/user/0/owned.filesystem/files","/data/data/owned.filesystem/files")){
            String path=prefix+"/Android/data/owned.filesystem/files/external.txt";
            equal(mapped(path),files.resolve("external.txt").toString());
            equal(mapped(path),HostFilesystem.mapPath(path));
            equal(new String(read(bytes(path)),StandardCharsets.UTF_8),"private storage");
            equal(mapped(prefix+"/Android/data/owned.filesystem/cache/cache.bin"),cache.resolve("cache.bin").toString());
            String ordinary=prefix+"/ordinary/Android/data/owned.filesystem/files/external.txt";
            equal(mapped(ordinary),files.resolve("ordinary/Android/data/owned.filesystem/files/external.txt").toString());
            String other=prefix+"/Android/data/other.package/files/external.txt";
            equal(mapped(other),files.resolve("Android/data/other.package/files/external.txt").toString());
            try{read(bytes(other));throw new AssertionError("Another package alias exposed private bytes");}catch(java.io.IOException expected){}
        }
        String deviceProtected="/data/user_de/0/owned.filesystem/files/Android/data/owned.filesystem/files/external.txt";
        equal(mapped(deviceProtected),data.resolve("device_protected/files/Android/data/owned.filesystem/files/external.txt").toString());
        String foreign="/data/user/0/other.package/files/Android/data/owned.filesystem/files/external.txt";
        if(mapped(foreign).equals(files.resolve("external.txt").toString()))throw new AssertionError("Foreign private root became an external alias");
        equal(mapped("/sdcard-other/Android/data/owned.filesystem/files/external.txt"),"/sdcard-other/Android/data/owned.filesystem/files/external.txt");
        for(String path:List.of("/data/../system/build.prop","/sdcard/Android/obb/owned.filesystem/main.obb","/sdcard/Android/data/owned.filesystem/cache/cache.bin","/sdcard/Android/data/owned.filesystem/files/../cache/cache.bin","/sdcard//Android/data/owned.filesystem/cache/cache.bin","/sdcard/literal\\filename"))equal(mapped(path),HostFilesystem.mapPath(path));
        equal(mapped("/sdcard-not-storage/file"),"/sdcard-not-storage/file");
        String longMissing=base+"/assets/"+"a".repeat(1400);equal(mapped(longMissing),HostFilesystem.mapPath(longMissing));
        equal(new String(resolve(bytes(base+"/assets/"+asset),8),StandardCharsets.UTF_8),base+"/assets/"+asset);
        var workers=new ArrayList<Thread>();var errors=new java.util.concurrent.ConcurrentLinkedQueue<Throwable>();
        for(int i=0;i<4;i++){Thread thread=new Thread(()->{try{for(int j=0;j<1000;j++)equal(mapped(base+"/assets/"+asset),assets.resolve(asset).toString());}catch(Throwable error){errors.add(error);}});workers.add(thread);thread.start();}
        for(int i=0;i<30;i++)HostFilesystem.installNativeMappings(List.of(base,split),assets);
        for(Thread thread:workers)thread.join();if(!errors.isEmpty())throw new AssertionError(errors.peek());
        HostFilesystem.installNativeMappings(List.of(base),assets);equal(mapped(split+"/assets/"+asset),split+"/assets/"+asset);
        System.out.println("PASS shared Java/native namespace, base and split assets, raw ZIP reads, UTF-8 paths and concurrent mapping replacement");
    }
}
