import java.nio.file.*;
import java.util.*;
import java.util.zip.ZipFile;
import org.objectweb.asm.*;

/** Build input for publisher registration: actual upstream and adapted native declarations. */
public final class FrameworkNativeDeclarations {
    private record Declaration(int natives){}
    private static Map<String,Declaration> declarations(Path archive)throws Exception{
        var result=new TreeMap<String,Declaration>();
        try(var input=new ZipFile(archive.toFile())){
            for(var entry:Collections.list(input.entries())){
                if(!entry.getName().endsWith(".class"))continue;
                try(var stream=input.getInputStream(entry)){
                    var reader=new ClassReader(stream);int[] natives={0};
                    reader.accept(new ClassVisitor(Opcodes.ASM9){
                        @Override public MethodVisitor visitMethod(int access,String name,String descriptor,String signature,String[] exceptions){
                            if((access&Opcodes.ACC_NATIVE)!=0)natives[0]++;return null;
                        }
                    },ClassReader.SKIP_CODE|ClassReader.SKIP_DEBUG|ClassReader.SKIP_FRAMES);
                    result.put(reader.getClassName(),new Declaration(natives[0]));
                }
            }
        }
        return result;
    }
    private static Map<String,Integer> families(Map<String,Declaration> classes){
        var result=new TreeMap<String,Integer>();
        for(var entry:classes.entrySet()){
            String owner=entry.getKey();
            for(;;){result.merge(owner,entry.getValue().natives(),Integer::sum);int nested=owner.lastIndexOf('$');if(nested<0)break;owner=owner.substring(0,nested);}
        }
        return result;
    }
    public static void main(String[] args)throws Exception{
        if(args.length!=3)throw new IllegalArgumentException("Expected original framework, adapted framework and declaration output");
        var original=declarations(Path.of(args[0]));var adapted=declarations(Path.of(args[1]));
        var before=families(original);var after=families(adapted);var output=new StringBuilder();
        var missing=new HashSet<String>();
        for(String name:original.keySet())if(!adapted.containsKey(name))for(;;){missing.add(name);int nested=name.lastIndexOf('$');if(nested<0)break;name=name.substring(0,nested);}
        for(var entry:before.entrySet()){
            // Missing classes never authorize omitting a supplied registration.
            output.append(entry.getKey()).append('\t').append(entry.getValue()).append('\t').append(missing.contains(entry.getKey())?-1:after.getOrDefault(entry.getKey(),0)).append('\n');
        }
        Files.writeString(Path.of(args[2]),output);
    }
}
