import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;

/** Exact native process/window ownership seams; original Android UI algorithms remain intact. */
final class UiPortability {
    static byte[] transform(String name,byte[] original){
        if(name.equals("android/animation/PropertyValuesHolder.class"))return propertyMethodAccess(original);
        if(!name.equals("android/hardware/display/DisplayManagerGlobal$DisplayListenerDelegate.class")&&!name.equals("android/view/ViewRootImpl.class"))return original;
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);boolean found=UiHookClassFiles.apply(node);
        for(MethodNode method:node.methods){
            if(!name.equals("android/hardware/display/DisplayManagerGlobal$DisplayListenerDelegate.class")||!method.name.equals("handleDisplayEventInner")||!method.desc.equals("(IILandroid/view/DisplayInfo;Z)V"))continue;
            found=true;LabelNode start=new LabelNode(),end=new LabelNode(),handler=new LabelNode(),admit=new LabelNode(),done=new LabelNode();
            for(AbstractInsnNode instruction:method.instructions.toArray())if(instruction.getOpcode()==Opcodes.RETURN)method.instructions.set(instruction,new JumpInsnNode(Opcodes.GOTO,done));
            InsnList enter=new InsnList();enter.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostDisplay","enterCallback","()Z",false));enter.add(new JumpInsnNode(Opcodes.IFNE,admit));enter.add(new InsnNode(Opcodes.RETURN));enter.add(admit);enter.add(start);method.instructions.insert(enter);
            method.instructions.add(end);method.instructions.add(done);method.instructions.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostDisplay","leaveCallback","()V",false));method.instructions.add(new InsnNode(Opcodes.RETURN));
            method.instructions.add(handler);int slot=method.maxLocals++;method.instructions.add(new VarInsnNode(Opcodes.ASTORE,slot));method.instructions.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostDisplay","leaveCallback","()V",false));method.instructions.add(new VarInsnNode(Opcodes.ALOAD,slot));method.instructions.add(new InsnNode(Opcodes.ATHROW));method.tryCatchBlocks.add(new TryCatchBlockNode(start,end,handler,null));
        }
        if(!found)throw new IllegalStateException("Android UI platform boundary changed: "+name);
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_FRAMES|ClassWriter.COMPUTE_MAXS);node.accept(writer);return writer.toByteArray();
    }

    /** Robolectric ShadowPropertyValuesHolder's ART/JVM access adaptation.
     * Keep AOSP's complete lookup, numeric conversion and Method caches. ART
     * permits public accessors on nonpublic target classes; HotSpot requires
     * making the same returned Method accessible before invoking it. */
    private static byte[] propertyMethodAccess(byte[] original){
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);boolean found=false;
        for(MethodNode method:node.methods){
            if(!method.name.equals("setupSetterOrGetter")||!method.desc.equals("(Ljava/lang/Class;Ljava/util/HashMap;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/reflect/Method;"))continue;
            for(AbstractInsnNode instruction:method.instructions.toArray())if(instruction.getOpcode()==Opcodes.ARETURN){
                LabelNode done=new LabelNode();InsnList access=new InsnList();
                access.add(new InsnNode(Opcodes.DUP));access.add(new JumpInsnNode(Opcodes.IFNULL,done));
                access.add(new InsnNode(Opcodes.DUP));access.add(new MethodInsnNode(Opcodes.INVOKEVIRTUAL,"java/lang/reflect/Method","getModifiers","()I",false));
                access.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"java/lang/reflect/Modifier","isPublic","(I)Z",false));access.add(new JumpInsnNode(Opcodes.IFEQ,done));
                access.add(new InsnNode(Opcodes.DUP));access.add(new InsnNode(Opcodes.ICONST_1));
                access.add(new MethodInsnNode(Opcodes.INVOKEVIRTUAL,"java/lang/reflect/Method","setAccessible","(Z)V",false));access.add(done);
                method.instructions.insertBefore(instruction,access);found=true;
            }
        }
        if(!found)throw new IllegalStateException("Android animation reflection boundary changed");
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_FRAMES|ClassWriter.COMPUTE_MAXS);node.accept(writer);return writer.toByteArray();
    }
}
