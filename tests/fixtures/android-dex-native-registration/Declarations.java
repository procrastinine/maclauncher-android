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
  // The negative control represents the historical Java-body conversion,
  // independent of whether the installed runtime already includes the repair.
  var broken=new ClassNode();new ClassReader(adapted).accept(broken,0);
  var method=broken.methods.stream().filter(m->m.name.equals("getClassNameList")).findFirst().orElseThrow();
  method.access &= ~Opcodes.ACC_NATIVE;
  method.instructions.add(new VarInsnNode(Opcodes.ALOAD,0));
  method.instructions.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostDexFiles","getClassNameList",method.desc,false));
  method.instructions.add(new InsnNode(Opcodes.ARETURN));
  var writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);broken.accept(writer);
  Path negative=Path.of(args[2],"dalvik/system/DexFile.class");Files.createDirectories(negative.getParent());Files.write(negative,writer.toByteArray());
  System.out.println("PASS all original DexFile members, bodies and 20 native declarations retained");
 }
}
