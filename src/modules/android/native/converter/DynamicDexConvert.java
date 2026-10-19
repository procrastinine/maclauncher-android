package io.github.kksimp.dex2jvm;

import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.Opcodes;

/** One isolated conversion request. Never defines or initializes guest classes. */
public final class DynamicDexConvert {
    /** Supplies the original converter's header resource API from the actual
     * running parent. ASM and conversion remain in this isolated process. */
    static final class ParentHeaders extends URLClassLoader {
        private final DataInputStream replies;
        private final boolean framed;
        private final DataOutputStream requests;
        private final Map<String,URL> resolved=new HashMap<>();
        ParentHeaders(URL[] urls,OutputStream requests){this(urls,requests,new DataInputStream(System.in),false);}
        ParentHeaders(URL[] urls,OutputStream requests,DataInputStream replies,boolean framed){super(urls,ClassLoader.getPlatformClassLoader());this.requests=new DataOutputStream(requests);this.replies=replies;this.framed=framed;}
        @Override public synchronized URL findResource(String name){
            if(!name.endsWith(".class"))return super.findResource(name);
            if(resolved.containsKey(name))return resolved.get(name);
            try{
                if(framed)requests.writeByte(1);requests.writeUTF(name.substring(0,name.length()-6));requests.flush();
                int size=replies.readInt();if(size<0)throw new IOException("Invalid parent class header length");
                byte[] data=replies.readNBytes(size);if(data.length!=size)throw new EOFException("Parent class header was truncated");
                URL resource;
                if(size==0)resource=super.findResource(name);
                else{
                    ClassWriter header=new ClassWriter(0);
                    try(DataInputStream facts=new DataInputStream(new ByteArrayInputStream(data))){
                        int access=facts.readInt();String type=facts.readUTF(),superclass=facts.readUTF();
                        String[] interfaces=new String[facts.readInt()];for(int i=0;i<interfaces.length;i++)interfaces[i]=facts.readUTF();
                        header.visit(Opcodes.V1_8,access,type,null,superclass.isEmpty()?null:superclass,interfaces);header.visitEnd();
                    }
                    byte[] bytes=header.toByteArray();
                    resource=new URL(null,"dex-parent:/"+name,new URLStreamHandler(){
                        @Override protected URLConnection openConnection(URL url){return new URLConnection(url){
                            @Override public void connect(){}
                            @Override public InputStream getInputStream(){return new ByteArrayInputStream(bytes);}
                        };}
                    });
                }
                resolved.put(name,resource);return resource;
            }catch(IOException failure){throw new UncheckedIOException("Cannot read dynamic parent hierarchy",failure);}
        }
    }
    static List<byte[]> readInputs(Path request)throws Exception {
        List<byte[]> dexes=new ArrayList<>();
        for(int i=0;;i++){
            Path file=request.resolve("input-"+i+".dex");if(!Files.exists(file))break;
            long size=Files.size(file);
            if(size<112)throw new IOException("Dynamic DEX header is truncated");
            byte[] data=Files.readAllBytes(file);
            if(ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getInt(32)!=data.length)
                throw new IOException("DEX file length does not match its header");
            Adler32 checksum=new Adler32();checksum.update(data,12,data.length-12);
            if(Integer.toUnsignedLong(ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getInt(8))!=checksum.getValue())
                throw new IOException("DEX checksum mismatch");
            MessageDigest signature=MessageDigest.getInstance("SHA-1");signature.update(data,32,data.length-32);
            if(!Arrays.equals(Arrays.copyOfRange(data,12,32),signature.digest()))throw new IOException("DEX signature mismatch");
            dexes.add(data);
        }
        if(dexes.isEmpty())throw new IOException("Dynamic DEX input is empty");
        return dexes;
    }
    public static void main(String[] args) throws Exception {
        boolean parentHeaders=args.length>1&&args[1].equals("--parent-headers");
        OutputStream requests=System.out;
        if(parentHeaders)System.setOut(System.err);
        Path request=Path.of(args[0]), output=request.resolve("classes.jar");
        List<byte[]> dexes=readInputs(request);
        List<URL> urls=new ArrayList<>();
        for(String line:Files.readAllLines(request.resolve("headers.txt"))){
            urls.add(Path.of(line).toUri().toURL());
        }
        try(URLClassLoader headers=parentHeaders?new ParentHeaders(urls.toArray(URL[]::new),requests):new URLClassLoader(urls.toArray(URL[]::new),ClassLoader.getPlatformClassLoader())){
            Options.hierarchyLoader=headers;Options.threads=2;Options.library="classloader";
            Options.syntheticPrefix="dex2jvm";Options.unsafeRedirect=null;Options.exitRedirect=null;
            Options.parsedMarker=false;Options.logTag="dynamic-dex";
            DexConverter.oversizeStubs.set(0);
            var session=DexConverter.open(dexes.toArray(byte[][]::new));
            try(ZipOutputStream zip=new ZipOutputStream(Files.newOutputStream(output,StandardOpenOption.CREATE_NEW))){
                session.forEach((name,data)->{
                    try{
                        ZipEntry entry=new ZipEntry(name+".class");entry.setTime(315532800000L);
                        zip.putNextEntry(entry);zip.write(data);zip.closeEntry();
                    }catch(IOException error){throw new UncheckedIOException(error);}
                });
            }
            // The converter already emits method-local diagnostics for JVM
            // bytecode limits. An unused SDK method must not discard the rest
            // of its valid module; unresolved classes fail only when requested.
            session.errors().forEach((name,failure)->System.err.println("[android-dex] Class could not be converted: "+name+": "+failure));
        }catch(Throwable error){Files.deleteIfExists(output);throw error;}
    }
}
