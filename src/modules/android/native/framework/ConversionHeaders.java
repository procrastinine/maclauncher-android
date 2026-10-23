import java.nio.file.*;
import java.util.*;
import java.util.zip.*;
import org.objectweb.asm.*;

/** Conversion-only declarations; executable framework archives remain intact. */
public final class ConversionHeaders {
    public static void main(String[] args)throws Exception {
        if(args.length<2)throw new IllegalArgumentException("Expected output and ordered framework archives");
        var classes=new TreeMap<String,byte[]>();
        var compileOnly=new TreeMap<String,byte[]>();
        String metadataPrefix="META-INF/maclauncher/compile-only/";
        for(int i=1;i<args.length;i++)try(var input=new ZipFile(args[i])){
            for(var entry:Collections.list(input.entries())){
                String name=entry.getName();boolean declarationOnly=name.startsWith(metadataPrefix);
                if(declarationOnly)name=name.substring(metadataPrefix.length());
                var declarations=declarationOnly?compileOnly:classes;
                if(!name.endsWith(".class")||declarations.containsKey(name))continue;
                try(var stream=input.getInputStream(entry)){
                    // A fresh constant pool excludes implementation-only strings,
                    // bytecode and line tables from the conversion cache identity.
                    var writer=new ClassWriter(0);
                    new ClassReader(stream).accept(writer,ClassReader.SKIP_CODE|ClassReader.SKIP_DEBUG|ClassReader.SKIP_FRAMES);
                    declarations.put(name,writer.toByteArray());
                }
            }
        }
        compileOnly.forEach(classes::putIfAbsent);
        try(var output=new ZipOutputStream(Files.newOutputStream(Path.of(args[0]),StandardOpenOption.CREATE_NEW))){
            for(var item:classes.entrySet()){
                var entry=new ZipEntry(item.getKey());entry.setTime(0);output.putNextEntry(entry);
                output.write(item.getValue());output.closeEntry();
            }
        }
    }
}
