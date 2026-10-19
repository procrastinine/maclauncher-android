package maclauncher.android.framework;

import java.io.*;
import java.util.*;
import java.util.zip.*;

/** HotSpot ZIP indexing below the original StrictJarFile verification and stream algorithms. */
public final class HostStrictJarFile {
    private HostStrictJarFile(){}
    private static long sequence=1;
    private static final Map<Long,Archive> archives=new HashMap<>();
    private static final Map<Long,Iteration> iterations=new HashMap<>();
    private record Archive(ZipFile zip){}
    private record Iteration(Archive archive,Enumeration<? extends ZipEntry> entries,String prefix){}
    private static final class Entry extends ZipEntry {
        final long dataOffset;
        Entry(ZipEntry source,long offset){super(source);dataOffset=offset;}
    }
    public static synchronized long nativeOpenJarFile(String name,int fd)throws IOException{
        // /dev/fd duplicates the already authorized descriptor, including an unlinked archive.
        ZipFile zip=new ZipFile("/dev/fd/"+fd);long handle=sequence++;archives.put(handle,new Archive(zip));return handle;
    }
    private static Archive archive(long handle)throws IOException{
        Archive value=archives.get(handle);if(value==null)throw new IOException("ZIP archive is closed");return value;
    }
    public static synchronized long nativeStartIteration(long handle,String prefix)throws IOException{
        Archive value=archive(handle);long id=sequence++;iterations.put(id,new Iteration(value,value.zip.entries(),Objects.requireNonNull(prefix)));return id;
    }
    private static ZipEntry entry(Archive archive,ZipEntry value)throws IOException{
        if(value==null)return null;
        try(InputStream stream=archive.zip.getInputStream(value)){return new Entry(value,entryDataOffset(stream));}
    }
    public static synchronized ZipEntry nativeNextEntry(long handle)throws IOException{
        Iteration value=iterations.get(handle);if(value==null)return null;
        while(value.entries.hasMoreElements()){
            ZipEntry next=value.entries.nextElement();if(next.getName().startsWith(value.prefix))return entry(value.archive,next);
        }
        iterations.remove(handle);return null;
    }
    public static synchronized ZipEntry nativeFindEntry(long handle,String name)throws IOException{
        Archive value=archive(handle);return entry(value,value.zip.getEntry(name));
    }
    public static synchronized void nativeClose(long handle)throws IOException{
        Archive value=archives.remove(handle);if(value==null)return;
        iterations.values().removeIf(item->item.archive==value);value.zip.close();
    }
    /** Android adds this metadata accessor to ZipEntry; Java 21 does not. */
    public static long dataOffset(ZipEntry value){
        if(value instanceof Entry entry)return entry.dataOffset;
        throw new IllegalArgumentException("ZIP entry does not belong to this Android archive");
    }
    private static native long entryDataOffset(InputStream stream)throws IOException;
}
