import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;

/** Mac buffer/compositor entrypoints; all upstream Java methods remain intact. */
final class SurfacePortability implements Opcodes {
    static byte[] transform(String name,byte[] bytes){
        if(name.equals("android/view/Choreographer.class"))return frameLifecycle(bytes);
        return bytes;
    }
    private static byte[] frameLifecycle(byte[] bytes){
        ClassNode type=new ClassNode();new ClassReader(bytes).accept(type,0);
        for(MethodNode method:type.methods){
            if(!method.name.equals("doFrame")||!method.desc.equals("(JILandroid/view/DisplayEventReceiver$VsyncEventData;)V"))continue;
            LabelNode admit=new LabelNode(),start=new LabelNode(),end=new LabelNode(),done=new LabelNode(),handler=new LabelNode();
            for(AbstractInsnNode instruction:method.instructions.toArray())if(instruction.getOpcode()==RETURN)method.instructions.set(instruction,new JumpInsnNode(GOTO,done));
            InsnList enter=new InsnList();enter.add(new MethodInsnNode(INVOKESTATIC,"maclauncher/android/framework/HostDisplayEvents","enterCallback","()Z",false));enter.add(new JumpInsnNode(IFNE,admit));enter.add(new InsnNode(RETURN));enter.add(admit);enter.add(start);method.instructions.insert(enter);
            method.instructions.add(end);method.instructions.add(done);method.instructions.add(new MethodInsnNode(INVOKESTATIC,"maclauncher/android/framework/HostDisplayEvents","leaveCallback","()V",false));method.instructions.add(new InsnNode(RETURN));
            method.instructions.add(handler);int error=method.maxLocals++;method.instructions.add(new VarInsnNode(ASTORE,error));method.instructions.add(new MethodInsnNode(INVOKESTATIC,"maclauncher/android/framework/HostDisplayEvents","leaveCallback","()V",false));method.instructions.add(new VarInsnNode(ALOAD,error));method.instructions.add(new InsnNode(ATHROW));method.tryCatchBlocks.add(new TryCatchBlockNode(start,end,handler,null));
        }
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS|ClassWriter.COMPUTE_FRAMES);type.accept(writer);return writer.toByteArray();
    }
}
