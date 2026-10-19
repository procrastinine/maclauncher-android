import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;

/** Host lifetime hooks around original Android background-task algorithms. */
final class TaskPortability implements Opcodes {
    static byte[] transform(String name,byte[] bytes){
        if(!name.equals("android/os/AsyncTask.class"))return bytes;
        ClassNode type=new ClassNode();new ClassReader(bytes).accept(type,0);
        for(MethodNode method:type.methods){
            if(method.name.equals("execute")&&(method.access&ACC_STATIC)!=0)
                method.instructions.insert(new MethodInsnNode(INVOKESTATIC,"maclauncher/android/framework/HostAsyncTasks","attachExecutors","()V",false));
            String hook=method.name.equals("<init>")?"attach":method.name.equals("finish")?"finished":null;
            if(hook==null)continue;
            for(AbstractInsnNode instruction:method.instructions.toArray())if(instruction.getOpcode()==RETURN){
                InsnList call=new InsnList();call.add(new VarInsnNode(ALOAD,0));call.add(new MethodInsnNode(INVOKESTATIC,"maclauncher/android/framework/HostAsyncTasks",hook,"(Landroid/os/AsyncTask;)V",false));method.instructions.insertBefore(instruction,call);
            }
        }
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);type.accept(writer);return writer.toByteArray();
    }
}
