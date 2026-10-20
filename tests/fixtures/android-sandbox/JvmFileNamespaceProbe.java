package maclauncher.android.framework;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import android.os.FileObserver;

/** Actual JDK native filesystem calls consume the installed Android aliases. */
public final class JvmFileNamespaceProbe {
    private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    public static void main(String[] args)throws Exception{
        Path runtime=Path.of(args[0]),data=Path.of(args[1]),files=data.resolve("files"),cache=data.resolve("cache");
        String pkg="org.example.namespace",name="main.12."+pkg+".obb",text="original expansion bytes é 🐈";
        HostFilesystem.configure(runtime,data,files,cache,pkg);
        System.setProperty("klepton.bridge",Path.of(args[2]).toAbsolutePath().toString());
        System.load(System.getProperty("klepton.bridge"));
        HostFilesystem.installNativeMappings(List.of(),files);
        File expansion=files.resolve("Android/obb/"+pkg+"/"+name).toFile();
        check(!expansion.exists(),"Fixture must not create physical alias copies");
        HostFileObserver.configure(data.toString());
        if(args.length>3&&args[3].equals("before")){
            check(!expansion.exists(),"Prior boundary unexpectedly maps direct JDK File calls");
            System.out.println("REPRODUCED direct JDK expansion alias is absent despite retained bytes");return;
        }
        File memory=new File("/proc/meminfo");
        check(memory.isFile()&&memory.canRead()&&Files.isRegularFile(memory.toPath()),"Virtual process metadata differs between File and NIO");
        try(var input=new RandomAccessFile(memory,"r")){check(input.readLine().startsWith("MemTotal:"),"RandomAccessFile bypasses virtual process files");input.seek(0);check(input.readLine().startsWith("MemTotal:"),"Virtual process descriptor cannot seek");}
        try(var input=new FileInputStream(memory)){check(new String(input.readNBytes(9),java.nio.charset.StandardCharsets.UTF_8).equals("MemTotal:"),"FileInputStream bypasses virtual process files");}
        check(Files.readString(memory.toPath()).startsWith("MemTotal:"),"NIO bypasses virtual process files");
        check(Files.readString(Path.of("/sys/devices/system/cpu/online")).trim().matches("[0-9]+(-[0-9]+)?"),"Virtual device file is absent");
        try(var ignored=new RandomAccessFile("/proc/maclauncher-missing-file","r")){throw new AssertionError("Missing process file was synthesized");}catch(FileNotFoundException expected){}
        check(expansion.exists()&&expansion.isFile()&&expansion.canRead(),"Direct File must resolve retained expansion");
        check(Files.exists(expansion.toPath())&&Files.isRegularFile(expansion.toPath()),"NIO expansion metadata differs");
        check(expansion.length()==Files.size(data.resolve("obb/"+name)),"Expansion size differs");
        check(Files.size(expansion.toPath())==expansion.length(),"NIO expansion size differs");
        try(var input=new FileInputStream(expansion)){check(new String(input.readAllBytes(),java.nio.charset.StandardCharsets.UTF_8).equals(text),"FileInputStream differs");}
        try(var input=new RandomAccessFile(expansion,"r")){check(input.length()==expansion.length(),"RandomAccessFile differs");}
        try(var input=Files.newInputStream(expansion.toPath())){check(input.readAllBytes().length==expansion.length(),"NIO open differs");}
        check(Arrays.asList(expansion.getParentFile().list()).contains(name),"Directory enumeration differs");
        check(expansion.getCanonicalFile().equals(data.resolve("obb/"+name).toFile().getCanonicalFile()),"Canonical ownership differs");
        check(expansion.getUsableSpace()>0&&expansion.getTotalSpace()>0,"Filesystem space is absent");
        var events=new LinkedBlockingQueue<Integer>();
        FileObserver observer=new FileObserver(expansion,FileObserver.OPEN|FileObserver.ACCESS|FileObserver.CLOSE_NOWRITE){
            @Override public void onEvent(int flags,String ignored){events.add(flags);}
        };
        observer.startWatching();
        try{
            for(int round=0;round<32;round++){
                events.clear();
                try(var input=new FileInputStream(expansion)){check(input.read()>=0,"Observed stream read differs");}
                int observed=0,wanted=FileObserver.OPEN|FileObserver.ACCESS|FileObserver.CLOSE_NOWRITE;
                long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
                while((observed&wanted)!=wanted&&System.nanoTime()<deadline){Integer next=events.poll(100,TimeUnit.MILLISECONDS);if(next!=null)observed|=next;}
                check((observed&wanted)==wanted,"Mapped JDK stream descriptor events are absent");
            }
        }finally{observer.stopWatching();HostFileObserver.shutdown();}
        File created=new File(expansion.getParentFile(),"unicode-é-🐈");
        check(created.mkdir()&&created.isDirectory(),"Directory creation differs");
        File original=new File(created,"output"),renamed=new File(created,"renamed");
        check(original.createNewFile(),"Exclusive creation differs");
        try(var output=new FileOutputStream(original)){output.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));}
        check(original.length()==text.getBytes(java.nio.charset.StandardCharsets.UTF_8).length,"Write size differs");
        long modified=1700000000000L;check(original.setLastModified(modified)&&original.lastModified()==modified,"Timestamp differs");
        check(original.setReadable(true,true)&&original.setWritable(true,true),"Permissions differ");
        check(original.renameTo(renamed)&&!original.exists()&&renamed.exists(),"Rename differs");
        check(renamed.setReadOnly()&&renamed.setWritable(true,true),"Read-only permission differs");
        check(renamed.delete()&&created.delete(),"Delete differs");
        Path ordinary=files.resolve("ordinary");Files.writeString(ordinary,"ordinary");
        check(ordinary.toFile().isFile()&&ordinary.toFile().delete(),"Ordinary JDK paths changed");
        File canonical=new File("/sdcard/Android/obb/"+pkg+"/"+name);
        check(canonical.exists()&&canonical.length()==expansion.length(),"Canonical Android alias differs");
        check(!new File("/sdcard/Android/obb/org.example.other/"+name).exists(),"Another package became visible");
        System.out.println("PASS actual JDK File metadata, directories, streams and canonical expansion ownership with descriptor events and virtual process files");
    }
}
