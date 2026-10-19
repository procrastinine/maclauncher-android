import org.objectweb.asm.*;

/** The original fast-data algorithms use scoped arrays instead of ART heap addresses. */
final class CharsetPortability implements Opcodes {
    static byte[] transform(String name,byte[] original){
        String type=name.endsWith(".class")?name.substring(0,name.length()-6):name;
        if(!type.equals("com/android/internal/util/ArtFastDataInput")&&!type.equals("com/android/internal/util/ArtFastDataOutput"))return original;
        ClassReader reader=new ClassReader(original);ClassWriter writer=new ClassWriter(reader,ClassWriter.COMPUTE_MAXS);
        reader.accept(new ClassVisitor(ASM9,writer){
            @Override public MethodVisitor visitMethod(int access,String name,String descriptor,String signature,String[] exceptions){
                return new MethodVisitor(ASM9,super.visitMethod(access,name,descriptor,signature,exceptions)){
                    @Override public void visitFieldInsn(int opcode,String owner,String field,String desc){
                        if(opcode==GETFIELD&&owner.equals(type)&&field.equals("mBufferPtr")&&desc.equals("J"))
                            super.visitFieldInsn(opcode,owner,"mBuffer","[B");
                        else super.visitFieldInsn(opcode,owner,field,desc);
                    }
                    @Override public void visitMethodInsn(int opcode,String owner,String method,String desc,boolean iface){
                        if(opcode==INVOKEVIRTUAL&&owner.equals("dalvik/system/VMRuntime")&&method.equals("addressOf")&&desc.equals("(Ljava/lang/Object;)J")){
                            if(name.equals("<init>")){
                                // Retain the private upstream field and constructor layout;
                                // consumers below use its existing Java array owner.
                                super.visitInsn(POP2);super.visitInsn(LCONST_0);
                            }else{super.visitInsn(SWAP);super.visitInsn(POP);}
                        }else if(opcode==INVOKESTATIC&&owner.equals("android/util/CharsetUtils")&&
                            ((method.equals("toModifiedUtf8Bytes")&&desc.equals("(Ljava/lang/String;JII)I"))||
                            (method.equals("fromModifiedUtf8Bytes")&&desc.equals("(JII)Ljava/lang/String;")))){
                            super.visitMethodInsn(opcode,"maclauncher/android/framework/HostCharset",method,desc.replace("J","[B"),false);
                        }else super.visitMethodInsn(opcode,owner,method,desc,iface);
                    }
                };
            }
        },0);return writer.toByteArray();
    }
}
