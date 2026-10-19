package io.github.kksimp.dex2jvm;

import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.util.*;

/** Resident public-converter session. Defines no guest classes and owns no platform services. */
public final class DemandDexConvert {
    public static void main(String[] args)throws Exception {
        Path request=Path.of(args[0]);
        var urls=new ArrayList<URL>();
        for(String line:Files.readAllLines(request.resolve("headers.txt")))urls.add(Path.of(line).toUri().toURL());
        // stdout is the byte protocol; upstream diagnostics stay on stderr.
        var output=new DataOutputStream(new BufferedOutputStream(System.out));System.setOut(System.err);
        try(var input=new DataInputStream(new BufferedInputStream(System.in));
            var headers=new DynamicDexConvert.ParentHeaders(urls.toArray(URL[]::new),output,input,true)){
            Options.hierarchyLoader=headers;Options.library="classloader";Options.threads=1;
            Options.syntheticPrefix="dex2jvm";Options.unsafeRedirect=null;Options.exitRedirect=null;
            Options.parsedMarker=false;Options.logTag="demand-dex";
            byte[][] dexes=Files.exists(request.resolve("sources.txt"))?ConvertApkSet.readSources(Files.readAllLines(request.resolve("sources.txt")).toArray(String[]::new)).dexes():DynamicDexConvert.readInputs(request).toArray(byte[][]::new);
            var session=DexConverter.open(dexes);
            // Preparation executes in this separate worker while the Android
            // process continues bootstrap, before it requests its first class.
            output.writeInt(1);output.writeInt(session.classNames().size());for(String name:session.classNames())output.writeUTF(name);output.flush();session.prepare();
            for(;;){
                String name;try{name=input.readUTF();}catch(EOFException closed){return;}
                byte[] bytes=session.classBytes(name);String failure=session.errors().get(name);
                output.writeByte(0);output.writeInt(bytes==null?0:bytes.length);if(bytes!=null)output.write(bytes);
                output.writeUTF(failure==null?"":failure);output.flush();
            }
        }
    }
}
