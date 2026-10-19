import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;

/** Native polling and host-loop ownership hooks; original Android algorithms remain intact. */
final class MessageQueuePortability implements Opcodes {
    private static final String HOST="maclauncher/android/framework/HostLooper",QUEUE="android/os/MessageQueue";
    static byte[] transform(String name,byte[] bytes){
        if(!name.equals(QUEUE+".class")&&!name.equals("android/os/Looper.class")&&!name.equals("android/os/HandlerThread.class"))return bytes;
        ClassNode type=new ClassNode();new ClassReader(bytes).accept(type,0);
        for(MethodNode method:type.methods){
            if(type.name.equals(QUEUE)){
                for(AbstractInsnNode instruction:method.instructions.toArray()){
                    if(instruction instanceof MethodInsnNode call&&call.owner.equals(QUEUE)&&call.name.equals("nativePollOnce")&&call.desc.equals("(JI)V")){
                        var replacement=new InsnList();LabelNode proceed=new LabelNode();
                        replacement.add(new MethodInsnNode(INVOKESTATIC,HOST,"poll","(Landroid/os/MessageQueue;JI)Z",false));replacement.add(new JumpInsnNode(IFNE,proceed));replacement.add(new InsnNode(ACONST_NULL));replacement.add(new InsnNode(ARETURN));replacement.add(proceed);method.instructions.insertBefore(call,replacement);method.instructions.remove(call);
                    }else if(instruction instanceof FieldInsnNode field&&field.getOpcode()==GETFIELD&&field.name.equals("mQuitAllowed")&&field.owner.equals(QUEUE)){
                        method.instructions.insertBefore(field,new InsnNode(DUP));method.instructions.insert(field,new MethodInsnNode(INVOKESTATIC,HOST,"quitAllowed","(Landroid/os/MessageQueue;Z)Z",false));
                    }else if(method.name.equals("<init>")&&instruction.getOpcode()==RETURN){
                        var hook=new InsnList();hook.add(new VarInsnNode(ALOAD,0));hook.add(new MethodInsnNode(INVOKESTATIC,HOST,"attachQueue","(Landroid/os/MessageQueue;)V",false));method.instructions.insertBefore(instruction,hook);
                    }else if(method.name.equals("next")&&method.desc.equals("()Landroid/os/Message;")&&instruction.getOpcode()==ARETURN){
                        var hook=new InsnList();hook.add(new VarInsnNode(ALOAD,0));hook.add(new InsnNode(SWAP));hook.add(new MethodInsnNode(INVOKESTATIC,HOST,"afterNext","(Landroid/os/MessageQueue;Landroid/os/Message;)Landroid/os/Message;",false));method.instructions.insertBefore(instruction,hook);
                    }
                }
            }else if(type.name.equals("android/os/Looper")&&method.name.equals("<init>")){
                for(AbstractInsnNode instruction:method.instructions.toArray())if(instruction.getOpcode()==RETURN){var hook=new InsnList();hook.add(new VarInsnNode(ALOAD,0));hook.add(new MethodInsnNode(INVOKESTATIC,HOST,"attachLooper","(Landroid/os/Looper;)V",false));method.instructions.insertBefore(instruction,hook);}
            }else if((type.name.equals("android/os/Looper")&&method.name.equals("loop")||type.name.equals("android/os/HandlerThread")&&method.name.equals("run"))&&method.desc.equals("()V")){
                LabelNode start=new LabelNode(),end=new LabelNode(),handler=new LabelNode();method.instructions.insert(start);
                for(AbstractInsnNode instruction:method.instructions.toArray())if(instruction.getOpcode()==RETURN)method.instructions.insertBefore(instruction,new MethodInsnNode(INVOKESTATIC,HOST,"loopExit","()V",false));
                method.instructions.add(end);method.instructions.add(handler);method.instructions.add(new MethodInsnNode(INVOKESTATIC,HOST,"loopFailed","(Ljava/lang/Throwable;)Ljava/lang/Throwable;",false));method.instructions.add(new InsnNode(ATHROW));method.tryCatchBlocks.add(new TryCatchBlockNode(start,end,handler,"java/lang/Throwable"));
            }
        }
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_FRAMES|ClassWriter.COMPUTE_MAXS);type.accept(writer);return writer.toByteArray();
    }
}
