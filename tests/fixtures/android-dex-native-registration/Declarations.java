import java.nio.file.*;
import java.util.*;
import java.util.zip.*;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
public final class Declarations {
 public static void main(String[] args)throws Exception {
  byte[] original;
  try(var zip=new ZipFile(args[0]);var input=zip.getInputStream(zip.getEntry("dalvik/system/DexFile.class"))){original=input.readAllBytes();}
  byte[] adapted=NativeBindings.transform(original);
  if(!Arrays.equals(original,adapted))throw new AssertionError("DexFile native declarations or original Java changed");
  var node=new ClassNode();new ClassReader(adapted).accept(node,0);
  long natives=node.methods.stream().filter(m->(m.access&Opcodes.ACC_NATIVE)!=0).count();
  if(natives!=20)throw new AssertionError("Original native family changed: "+natives);
  Path file=Path.of(args[1],"dalvik/system/DexFile.class");Files.createDirectories(file.getParent());Files.write(file,adapted);
  System.out.println("PASS all original DexFile members, bodies and 20 native declarations retained");
 }
}
