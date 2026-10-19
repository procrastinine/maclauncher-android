import java.io.*;
import java.util.*;
import java.util.zip.ZipFile;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;

/** JVM RegisterNatives initializes classes before their native entries are bound. */
final class GraphicsPortability {
    private static List<ZipFile> frameworks=List.of();
    private static final Map<String,ClassNode> declarations=new HashMap<>();
    private static final Map<String,Boolean> nativeDependencies=new HashMap<>();
    private static java.util.function.BiFunction<String,byte[],byte[]> normalize=(name,bytes)->bytes;
    static void initialize(ZipFile input){initialize(input,(name,bytes)->bytes);}
    static void initialize(ZipFile input,java.util.function.BiFunction<String,byte[],byte[]> adapter){initialize(List.of(input),adapter);}
    static void initialize(List<ZipFile> inputs,java.util.function.BiFunction<String,byte[],byte[]> adapter){frameworks=List.copyOf(inputs);normalize=adapter;declarations.clear();nativeDependencies.clear();}
    private static ClassNode declaration(String owner){
        // java.* is supplied and initialized by HotSpot, not the Android JNI library.
        if(owner==null||owner.startsWith("java/"))return null;
        if(declarations.containsKey(owner))return declarations.get(owner);
        ClassNode node=null;
        for(var framework:frameworks){var entry=framework.getEntry(owner+".class");if(entry!=null){try(var input=framework.getInputStream(entry)){node=new ClassNode();new ClassReader(normalize.apply(entry.getName(),input.readAllBytes())).accept(node,ClassReader.SKIP_DEBUG|ClassReader.SKIP_FRAMES);}catch(IOException failure){throw new UncheckedIOException(failure);}break;}}
        declarations.put(owner,node);return node;
    }
    private record Method(ClassNode owner,MethodNode code){String key(){return owner.name+"."+code.name+code.desc;}}
    private static void method(ArrayDeque<Method> work,String owner,String name,String descriptor){
        ClassNode type=declaration(owner);
        while(type!=null){for(MethodNode method:type.methods)if(method.name.equals(name)&&method.desc.equals(descriptor)){work.add(new Method(type,method));return;}type=name.equals("<clinit>")?null:declaration(type.superName);}
    }
    private static boolean callsNative(ClassNode node,MethodNode method){
        Method start=new Method(node,method);Set<String> visited=new HashSet<>();var work=new ArrayDeque<Method>();work.add(start);
        while(!work.isEmpty()){
            Method current=work.removeFirst();if(!visited.add(current.key()))continue;
            Boolean known=nativeDependencies.get(current.key());if(Boolean.FALSE.equals(known))continue;
            if(Boolean.TRUE.equals(known)||(current.code.access&Opcodes.ACC_NATIVE)!=0){nativeDependencies.put(start.key(),true);return true;}
            for(AbstractInsnNode instruction:current.code.instructions){
                if(instruction instanceof MethodInsnNode call){
                    method(work,call.owner,call.name,call.desc);
                    if(call.getOpcode()==Opcodes.INVOKESTATIC&&!call.owner.equals(current.owner.name))method(work,call.owner,"<clinit>","()V");
                }else if(instruction instanceof FieldInsnNode field&&(field.getOpcode()==Opcodes.GETSTATIC||field.getOpcode()==Opcodes.PUTSTATIC)&&!field.owner.equals(current.owner.name))method(work,field.owner,"<clinit>","()V");
                else if(instruction instanceof TypeInsnNode type&&type.getOpcode()==Opcodes.NEW)method(work,type.desc,"<clinit>","()V");
            }
        }
        for(String key:visited)nativeDependencies.put(key,false);return false;
    }
    static byte[] transform(String name,byte[] original){
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);
        if(!declarations.containsKey(node.name)&&node.methods.stream().anyMatch(method->method.name.equals("<clinit>"))){ClassNode dependency=new ClassNode();new ClassReader(original).accept(dependency,ClassReader.SKIP_DEBUG|ClassReader.SKIP_FRAMES);declarations.put(node.name,dependency);}
        boolean changed=false;
        for(MethodNode method:List.copyOf(node.methods))if(method.name.equals("<clinit>")&&callsNative(node,method)){
            if((node.access&Opcodes.ACC_INTERFACE)!=0){
                // Interface fields must remain final, and only <clinit> may assign
                // them. Bootstrap native dependencies before the original body;
                // moving its stores to a helper would produce illegal bytecode.
                method.instructions.insert(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/JvmNativeMemory","initialize","()V",false));
                changed=true;continue;
            }
            // Preserve and execute the exact initializer once after registration.
            // JVM only permits final-field stores in <clinit>, so fields written
            // by this deferred initializer retain value/visibility but not final.
            for(AbstractInsnNode instruction:method.instructions)if(instruction instanceof FieldInsnNode field&&field.getOpcode()==Opcodes.PUTSTATIC&&field.owner.equals(node.name))
                for(FieldNode target:node.fields)if(target.name.equals(field.name)&&target.desc.equals(field.desc))target.access&=~Opcodes.ACC_FINAL;
            method.name="hostInitializeNatives";method.access=Opcodes.ACC_PUBLIC|Opcodes.ACC_STATIC|Opcodes.ACC_SYNTHETIC;
            MethodNode initializer=new MethodNode(Opcodes.ACC_STATIC,"<clinit>","()V",null,null);
            initializer.instructions.add(new LdcInsnNode(Type.getObjectType(node.name)));
            initializer.instructions.add(new MethodInsnNode(Opcodes.INVOKESTATIC,"maclauncher/android/framework/JvmNativeMemory","initializeClass","(Ljava/lang/Class;)V",false));
            initializer.instructions.add(new InsnNode(Opcodes.RETURN));node.methods.add(initializer);changed=true;
        }
        if(!changed)return original;
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);node.accept(writer);return writer.toByteArray();
    }
}
