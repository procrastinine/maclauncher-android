import org.objectweb.asm.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Original graphics APIs; registrations are derived from their native source. */
final class GlesPortability implements Opcodes {
 private static final Set<String> owners=registrations();
 private static Set<String> registrations(){
  try(var stream=GlesPortability.class.getResourceAsStream("/graphics-registrations.txt")){
   if(stream==null)return Set.of();
   return Set.copyOf(new String(stream.readAllBytes(),StandardCharsets.UTF_8).lines().filter(line->!line.isBlank()).toList());
  }catch(IOException error){throw new UncheckedIOException(error);}
 }
 static byte[] transform(String file,byte[] bytes){
  String owner=file.replace(".class","");
  if(owners.isEmpty()&&(owner.startsWith("android/opengl/")||owner.startsWith("com/google/android/gles_jni/")))throw new IllegalStateException("Missing generated graphics JNI registrations");
  if(!owners.contains(owner))return bytes;
  ClassReader reader=new ClassReader(bytes);ClassWriter writer=new ClassWriter(reader,ClassWriter.COMPUTE_MAXS);
  reader.accept(new ClassVisitor(ASM9,writer){
   boolean initialized;
   void register(MethodVisitor out){out.visitLdcInsn(Type.getObjectType(owner));out.visitMethodInsn(INVOKESTATIC,"maclauncher/android/framework/HostGles","register","(Ljava/lang/Class;)V",false);}
   @Override public MethodVisitor visitMethod(int access,String name,String descriptor,String signature,String[] exceptions){
    MethodVisitor out=super.visitMethod(access,name,descriptor,signature,exceptions);
    if(!name.equals("<clinit>"))return out;
    initialized=true;
    return new MethodVisitor(ASM9,out){@Override public void visitCode(){super.visitCode();register(this);}};
   }
   @Override public void visitEnd(){
    if(!initialized){MethodVisitor out=super.visitMethod(ACC_STATIC,"<clinit>","()V",null,null);out.visitCode();register(out);out.visitInsn(RETURN);out.visitMaxs(0,0);out.visitEnd();}
    super.visitEnd();
   }
  },0);
  return writer.toByteArray();
 }
}
