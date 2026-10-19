import org.objectweb.asm.*;

/** Retains AOSP path algorithms; filesystem roots and mount-service results come from the host. */
final class FilesystemPortability implements Opcodes {
    private static final String HOST="maclauncher/android/framework/HostFilesystem";
    static byte[] transform(String name,byte[] bytes){
        String type=name.endsWith(".class")?name.substring(0,name.length()-6):name;
        boolean environment=type.equals("android/os/Environment"),fonts=type.equals("android/graphics/fonts/SystemFonts");
        if(!environment&&!fonts)return bytes;
        var reader=new ClassReader(bytes);var writer=new ClassWriter(reader,ClassWriter.COMPUTE_MAXS);
        reader.accept(new ClassVisitor(ASM9,writer){
            @Override public MethodVisitor visitMethod(int access,String name,String descriptor,String signature,String[] exceptions){
                MethodVisitor output=super.visitMethod(access,name,descriptor,signature,exceptions);
                return new MethodVisitor(ASM9,output){
                    @Override public void visitInsn(int opcode){
                        if(environment&&opcode==ARETURN&&Type.getReturnType(descriptor).getDescriptor().equals("Ljava/io/File;"))super.visitMethodInsn(INVOKESTATIC,HOST,"file","(Ljava/io/File;)Ljava/io/File;",false);
                        if(environment&&opcode==ARETURN&&name.equals("getDirectoryPath")&&descriptor.equals("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"))super.visitMethodInsn(INVOKESTATIC,HOST,"mapPath","(Ljava/lang/String;)Ljava/lang/String;",false);
                        // These existing private getters are the upstream host path seam.
                        // Parsing, family construction and fallback ordering stay unchanged.
                        if(fonts&&opcode==ARETURN&&(name.equals("getFontsXmlDir")||name.equals("getSystemFontDir"))&&descriptor.equals("()Ljava/lang/String;"))super.visitMethodInsn(INVOKESTATIC,HOST,"directoryPath","(Ljava/lang/String;)Ljava/lang/String;",false);
                        super.visitInsn(opcode);
                    }
                };
            }
        },0);return writer.toByteArray();
    }
}
