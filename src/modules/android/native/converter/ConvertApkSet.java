package io.github.kksimp.dex2jvm;

import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** One class hierarchy and converter session across the selected, verified APKs. */
public final class ConvertApkSet {
    record Sources(byte[][] dexes,LinkedHashMap<String,Integer> counts){}
    static Sources readSources(String[] paths)throws IOException {
        var dexes=new ArrayList<byte[]>();var counts=new LinkedHashMap<String,Integer>();
        for(String path:paths)try(var zip=new ZipFile(path)){
            var entries=zip.stream().filter(e->e.getName().matches("classes(?:[2-9]|[1-9][0-9]+)?\\.dex"))
                .sorted(Comparator.comparingInt(e->e.getName().equals("classes.dex")?1:Integer.parseInt(e.getName().substring(7,e.getName().length()-4)))).toList();
            if(counts.putIfAbsent(Path.of(path).getFileName().toString(),entries.size())!=null)throw new IOException("Android source APK names are duplicated");
            for(var entry:entries){
                if(entry.getSize()<112)throw new IOException("Android bytecode header is truncated");
                try(var in=zip.getInputStream(entry)){
                    byte[] data=in.readAllBytes();CRC32 crc=new CRC32();crc.update(data);
                    if(data.length!=entry.getSize()||crc.getValue()!=entry.getCrc())throw new IOException("Invalid Android bytecode entry");
                    dexes.add(data);
                }
            }
        }
        return new Sources(dexes.toArray(byte[][]::new),counts);
    }
    /** Record the upstream converter's actual external hierarchy lookups. The
     * resulting bytecode can be reused by any equivalent runtime delegation. */
    private static final class Hierarchy extends URLClassLoader {
        final Set<String> dependencies=java.util.concurrent.ConcurrentHashMap.newKeySet();
        Hierarchy(URL[] urls){super(urls,ClassLoader.getPlatformClassLoader());}
        @Override public URL getResource(String name){
            if(name.endsWith(".class"))dependencies.add(name.substring(0,name.length()-6));
            return super.getResource(name);
        }
    }
    public static void main(String[] args) throws Exception {
        if(args.length<3)throw new IllegalArgumentException("Expected metadata, output and APK paths");
        Path output=Path.of(args[1]);Sources original=readSources(Arrays.copyOfRange(args,2,args.length));
        var dexes=original.dexes();var counts=original.counts();
        if(dexes.length==0){try(var zip=new ZipOutputStream(Files.newOutputStream(output,StandardOpenOption.CREATE_NEW))){}return;}
        var hierarchy=new ArrayList<URL>();
        for(String entry:args[0].split(java.util.regex.Pattern.quote(File.pathSeparator)))hierarchy.add(Path.of(entry).toUri().toURL());
        try(var loader=new Hierarchy(hierarchy.toArray(URL[]::new))){
            Options.hierarchyLoader=loader;Options.library="classloader";Options.threads=2;
            var errors=new LinkedHashMap<String,String>();DexConverter.oversizeStubs.set(0);
            var session=DexConverter.open(dexes);
            boolean demand=Boolean.getBoolean("maclauncher.dex.demand");
            if(demand){try(var zip=new ZipOutputStream(Files.newOutputStream(output,StandardOpenOption.CREATE_NEW))){};}
            else Main.convert(session,output,errors);
            // Preserve the upstream parser's source membership alongside the
            // combined hierarchy. Original DexPathList elements can then reuse
            // preparation without merging APK class or resource namespaces.
            var members=new LinkedHashMap<String,LinkedHashSet<String>>();var owners=new HashMap<String,String>();var duplicates=new HashSet<String>();int dexIndex=0;
            for(var source:counts.entrySet()){
                var names=new LinkedHashSet<String>();members.put(source.getKey(),names);
                for(int i=0;i<source.getValue();i++)for(var type:session.dexFiles().get(dexIndex++).classes()){
                    names.add(type.name());String previous=owners.putIfAbsent(type.name(),source.getKey());
                    if(previous!=null&&!previous.equals(source.getKey())){duplicates.add(previous);duplicates.add(source.getKey());}
                }
            }
            try(var archive=FileSystems.newFileSystem(output)){
                if(demand){
                    // The original DEX bytes, not just class names, identify this
                    // module's demand conversions across subsequent launches.
                    var digest=java.security.MessageDigest.getInstance("SHA-256");
                    for(byte[] dex:dexes){digest.update(java.nio.ByteBuffer.allocate(8).putLong(dex.length).array());digest.update(dex);}
                    Path marker=archive.getPath("/META-INF/maclauncher/demand-dex");Files.createDirectories(marker.getParent());
                    try(var data=new DataOutputStream(Files.newOutputStream(marker,StandardOpenOption.CREATE_NEW))){data.writeInt(1);data.write(digest.digest());}
                    Files.setLastModifiedTime(marker,java.nio.file.attribute.FileTime.fromMillis(315532800000L));
                }
                Path index=archive.getPath("/META-INF/maclauncher/apk-classes");Files.createDirectories(index.getParent());
                try(var data=new DataOutputStream(Files.newOutputStream(index,StandardOpenOption.CREATE_NEW))){
                    data.writeInt(1);data.writeInt(members.size());
                    for(var source:members.entrySet()){data.writeUTF(source.getKey());data.writeBoolean(!duplicates.contains(source.getKey()));data.writeInt(source.getValue().size());for(String name:source.getValue())data.writeUTF(name);}
                }
                Files.setLastModifiedTime(index,java.nio.file.attribute.FileTime.fromMillis(315532800000L));
                Path dependencies=archive.getPath("/META-INF/maclauncher/parent-classes");
                try(var data=new DataOutputStream(Files.newOutputStream(dependencies,StandardOpenOption.CREATE_NEW))){
                    data.writeInt(1);data.writeInt(loader.dependencies.size());
                    for(String name:new TreeSet<>(loader.dependencies))data.writeUTF(name);
                }
                Files.setLastModifiedTime(dependencies,java.nio.file.attribute.FileTime.fromMillis(315532800000L));
            }
            errors.forEach((name,failure)->System.err.println("[android-dex] Class could not be converted: "+name+": "+failure));
        }catch(Throwable error){Files.deleteIfExists(output);throw error;}
    }
}
