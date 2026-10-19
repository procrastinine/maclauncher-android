import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;
import java.io.*;
import java.util.*;
import java.util.zip.*;

/** JVM/ART boundaries only; complete upstream loader and DexPathList algorithms. */
final class DexPortability implements Opcodes {
    private static final String HOST="maclauncher/android/framework/HostDexFiles";
    private static Set<String> parallelLoaders=Set.of();
    /** ART does not require HotSpot's per-subclass opt-in. Derive the complete
     * original DEX-loader hierarchy rather than enabling only our final subclass. */
    static void initialize(Collection<ZipFile> inputs)throws IOException{
        var parents=new HashMap<String,String>();
        for(var archive:inputs)for(var entry:Collections.list(archive.entries())){
            String name=entry.getName();if(!name.startsWith("dalvik/system/")||!name.endsWith(".class"))continue;
            try(var input=archive.getInputStream(entry)){var type=new ClassReader(input);parents.put(type.getClassName(),type.getSuperName());}
        }
        var loaders=new HashSet<String>();
        for(String name:parents.keySet()){
            var visited=new HashSet<String>();
            for(String at=name;at!=null&&visited.add(at);at=parents.get(at))
                if("java/lang/ClassLoader".equals(at)){loaders.add(name);break;}
        }
        parallelLoaders=Set.copyOf(loaders);
    }
    /** ART resolves descriptors in VMClassLoader before ordinary delegation.
     * HotSpot's bootstrap ClassLoader cannot be transformed with the Android
     * archive, so adapt that exact VM entry on the complete original hierarchy.
     * A reference array resolves its component through this actual loader. */
    private static InsnList classNameBoundary(){
        var code=new InsnList();var ordinary=new LabelNode();
        code.add(new VarInsnNode(ALOAD,1));
        code.add(new MethodInsnNode(INVOKESTATIC,HOST,"className","(Ljava/lang/String;)Ljava/lang/String;",false));
        code.add(new VarInsnNode(ASTORE,1));
        code.add(new VarInsnNode(ALOAD,1));code.add(new VarInsnNode(ALOAD,0));
        code.add(new MethodInsnNode(INVOKESTATIC,HOST,"arrayClass","(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/Class;",false));
        code.add(new InsnNode(DUP));code.add(new JumpInsnNode(IFNULL,ordinary));code.add(new InsnNode(ARETURN));
        code.add(ordinary);code.add(new FrameNode(F_SAME1,0,null,1,new Object[]{"java/lang/Class"}));code.add(new InsnNode(POP));
        return code;
    }
    static byte[] transform(String name,byte[] bytes){
        if(!name.startsWith("dalvik/system/")&&!name.startsWith("libcore/io/ClassPathURLStreamHandler"))return bytes;
        ClassNode type=new ClassNode();new ClassReader(bytes).accept(type,0);boolean changed=false;
        for(MethodNode method:type.methods){
            for(AbstractInsnNode instruction:method.instructions.toArray()){
                if(instruction instanceof MethodInsnNode call){
                    if(type.name.equals("dalvik/system/BaseDexClassLoader")&&call.owner.equals("java/lang/ClassLoader")&&call.name.equals("<init>")&&call.desc.equals("(Ljava/lang/ClassLoader;)V")){
                        method.instructions.insertBefore(call,new MethodInsnNode(INVOKESTATIC,HOST,"parent","(Ljava/lang/ClassLoader;)Ljava/lang/ClassLoader;",false));changed=true;
                    }else if(type.name.equals("dalvik/system/DelegateLastClassLoader")&&call.owner.equals("java/lang/Class")&&call.name.equals("getClassLoader")){
                        call.setOpcode(INVOKESTATIC);call.owner=HOST;call.name="bootClassLoader";call.desc="(Ljava/lang/Class;)Ljava/lang/ClassLoader;";changed=true;
                    }else if(call.owner.equals("java/util/jar/JarFile")&&call.name.equals("<init>")&&call.desc.equals("(Ljava/lang/String;ZZ)V")){
                        var removeValidator=new InsnList();removeValidator.add(new InsnNode(SWAP));removeValidator.add(new InsnNode(POP));method.instructions.insertBefore(call,removeValidator);call.desc="(Ljava/lang/String;Z)V";changed=true;
                    }else if(type.name.equals("dalvik/system/DexPathList")&&call.owner.equals("java/lang/System")&&call.name.equals("mapLibraryName")){
                        call.owner="maclauncher/android/bridge/GuestNativeLoader";changed=true;
                    }
                    if(call.owner.equals("sun/misc/CompoundEnumeration")){call.owner="maclauncher/android/framework/JvmCompoundEnumeration";changed=true;}
                }else if(instruction instanceof TypeInsnNode alloc&&alloc.desc.equals("sun/misc/CompoundEnumeration")){alloc.desc="maclauncher/android/framework/JvmCompoundEnumeration";changed=true;}
            }
        }
        if(parallelLoaders.contains(type.name)){
            var load=type.methods.stream().filter(method->method.name.equals("loadClass")&&method.desc.equals("(Ljava/lang/String;Z)Ljava/lang/Class;")).findFirst().orElse(null);
            if(load==null&&type.superName.equals("java/lang/ClassLoader")){
                load=new MethodNode(ACC_PROTECTED,"loadClass","(Ljava/lang/String;Z)Ljava/lang/Class;","(Ljava/lang/String;Z)Ljava/lang/Class<*>;",new String[]{"java/lang/ClassNotFoundException"});
                load.instructions.add(new VarInsnNode(ALOAD,0));load.instructions.add(new VarInsnNode(ALOAD,1));load.instructions.add(new VarInsnNode(ILOAD,2));
                load.instructions.add(new MethodInsnNode(INVOKESPECIAL,type.superName,"loadClass",load.desc,false));load.instructions.add(new InsnNode(ARETURN));type.methods.add(load);
            }
            if(load!=null){load.instructions.insert(classNameBoundary());changed=true;}
            MethodNode initializer=type.methods.stream().filter(method->method.name.equals("<clinit>")).findFirst().orElse(null);
            if(initializer==null){initializer=new MethodNode(ACC_STATIC,"<clinit>","()V",null,null);initializer.instructions.add(new InsnNode(RETURN));type.methods.add(initializer);}
            boolean registered=false;
            for(AbstractInsnNode instruction:initializer.instructions.toArray())if(instruction instanceof MethodInsnNode call&&call.getOpcode()==INVOKESTATIC&&(call.owner.equals(type.name)||call.owner.equals("java/lang/ClassLoader"))&&call.name.equals("registerAsParallelCapable")&&call.desc.equals("()Z")){registered=true;break;}
            if(!registered){var registration=new InsnList();registration.add(new MethodInsnNode(INVOKESTATIC,type.name,"registerAsParallelCapable","()Z",false));registration.add(new InsnNode(POP));initializer.instructions.insert(registration);changed=true;}
        }
        if(!changed)return bytes;ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);type.accept(writer);return writer.toByteArray();
    }
}
