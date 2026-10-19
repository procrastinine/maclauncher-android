import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
/** JDK stream fast paths must retain the original Android descriptor owner. */
final class DescriptorPortability {
    static byte[] transform(String name,byte[] original){
        if(!name.equals("android/os/ParcelFileDescriptor$AutoCloseInputStream.class"))return original;
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);
        for(String[] api:new String[][]{{"readAllBytes","()[B"},{"readNBytes","(I)[B"},{"transferTo","(Ljava/io/OutputStream;)J"}}){
            if(node.methods.stream().anyMatch(method->method.name.equals(api[0])&&method.desc.equals(api[1])))continue;
            MethodNode method=new MethodNode(Opcodes.ACC_PUBLIC,api[0],api[1],null,new String[]{"java/io/IOException"});method.instructions.add(new VarInsnNode(Opcodes.ALOAD,0));int slot=1;for(Type argument:Type.getArgumentTypes(api[1])){method.instructions.add(new VarInsnNode(argument.getOpcode(Opcodes.ILOAD),slot));slot+=argument.getSize();}
            method.instructions.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostSequentialInput",api[0],"(Ljava/io/InputStream;"+api[1].substring(1),false));method.instructions.add(new InsnNode(Type.getReturnType(api[1]).getOpcode(Opcodes.IRETURN)));node.methods.add(method);
        }
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);node.accept(writer);return writer.toByteArray();
    }
}
