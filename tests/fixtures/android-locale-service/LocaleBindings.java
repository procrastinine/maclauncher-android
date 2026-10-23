import java.nio.file.*;
import java.util.jar.*;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
/** Apply the production platform-call transformation to original supplied classes. */
public final class LocaleBindings {
 public static void main(String[] args)throws Exception{
  try(var jar=new JarFile(args[0])){for(var entries=jar.entries();entries.hasMoreElements();){var entry=entries.nextElement();
   if(!entry.getName().startsWith("com/android/server/locales/")||!entry.getName().endsWith(".class"))continue;
   byte[] bytes;try(var input=jar.getInputStream(entry)){bytes=input.readAllBytes();}byte[] adapted=ServicePortability.transform(entry.getName(),bytes);
   ClassNode before=new ClassNode(),after=new ClassNode();new ClassReader(bytes).accept(before,0);new ClassReader(adapted).accept(after,0);
   if(!before.methods.stream().map(m->m.name+m.desc+"/"+m.access).toList().equals(after.methods.stream().map(m->m.name+m.desc+"/"+m.access).toList()))throw new AssertionError("Original locale API changed");
   Path output=Path.of(args[1],entry.getName());Files.createDirectories(output.getParent());Files.write(output,adapted);
  }}
 }
}
