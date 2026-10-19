package io.github.kksimp.dex2jvm;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Build-time export of upstream's Apache-licensed hierarchy metadata. */
public final class ExportApiHeaders {
    public static void main(String[] args)throws Exception {
        if(args.length!=1)throw new IllegalArgumentException("Expected output metadata JAR");
        ApiIndexLoader loader=ApiIndexLoader.bundled();
        if(loader==null)throw new IOException("The converter has no bundled Android API metadata");
        SortedSet<String> names=new TreeSet<>();
        try(var raw=ApiIndexLoader.class.getResourceAsStream(ApiIndexLoader.RESOURCE)){
            if(raw==null)throw new IOException("Android API metadata is missing");
            try(var lines=new BufferedReader(new InputStreamReader(new GZIPInputStream(raw),StandardCharsets.UTF_8))){
                for(String line;(line=lines.readLine())!=null;){
                    if(line.startsWith("#")||line.isBlank())continue;
                    String[] fields=line.split(" ");
                    if(fields.length<3||!fields[1].matches("[A-Za-z_$][A-Za-z0-9_$/]*"))throw new IOException("Invalid Android API metadata entry");
                    names.add(fields[1]+".class");
                }
            }
        }
        if(names.size()!=loader.size()||names.size()<4000)throw new IOException("Android API metadata is incomplete");
        try(var zip=new ZipOutputStream(Files.newOutputStream(Path.of(args[0]),StandardOpenOption.CREATE_NEW))){
            for(String name:names){
                try(var input=loader.getResourceAsStream(name)){
                    if(input==null)throw new IOException("Android API header is missing: "+name);
                    ZipEntry entry=new ZipEntry(name);entry.setTime(0);zip.putNextEntry(entry);input.transferTo(zip);zip.closeEntry();
                }
            }
        }
        System.out.println(names.size()+" Android hierarchy headers exported; no SDK or executable framework code");
    }
}
