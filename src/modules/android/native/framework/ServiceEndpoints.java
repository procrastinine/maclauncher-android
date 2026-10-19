import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;

/** Generate typed delegates beneath the complete publisher AIDL Stub dispatchers. */
public final class ServiceEndpoints implements Opcodes {
    private final JarFile framework;
    private final Map<String,ClassNode> nodes=new HashMap<>();
    private ServiceEndpoints(Path archive)throws IOException{framework=new JarFile(archive.toFile());}
    private ClassNode node(String name)throws IOException{
        if(nodes.containsKey(name))return nodes.get(name);
        var entry=framework.getJarEntry(name+".class");if(entry==null){nodes.put(name,null);return null;}
        ClassNode value=new ClassNode();try(var stream=framework.getInputStream(entry)){new ClassReader(stream).accept(value,ClassReader.SKIP_CODE|ClassReader.SKIP_DEBUG|ClassReader.SKIP_FRAMES);}nodes.put(name,value);return value;
    }
    private boolean aidl(String name,Set<String> visited)throws IOException{
        if(name.equals("android/os/IInterface"))return true;if(!visited.add(name))return false;
        ClassNode node=node(name);if(node==null||(node.access&ACC_INTERFACE)==0)return false;
        for(String parent:node.interfaces)if(aidl(parent,visited))return true;return false;
    }
    private void methods(String name,Map<String,MethodNode> methods,Set<String> visited)throws IOException{
        if(!visited.add(name))return;ClassNode node=node(name);if(node==null)return;
        for(String parent:node.interfaces)methods(parent,methods,visited);
        for(MethodNode method:node.methods)if((method.access&(ACC_PUBLIC|ACC_STATIC))==ACC_PUBLIC)methods.put(method.name+method.desc,method);
    }
    private boolean concrete(String owner,String key)throws IOException{
        for(ClassNode value=node(owner);value!=null;value=value.superName==null?null:node(value.superName))for(MethodNode method:value.methods)if((method.name+method.desc).equals(key))return(method.access&ACC_ABSTRACT)==0;
        return false;
    }
    private void generate(String name,Path output)throws IOException{
        ClassNode stub=node(name+"$Stub");if(stub==null||!aidl(name,new HashSet<>()))return;
        String generated="maclauncher/android/framework/endpoints/"+name;
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);writer.visit(V21,ACC_PUBLIC|ACC_FINAL,generated,null,stub.name,null);
        writer.visitField(ACC_PRIVATE|ACC_FINAL,"target","L"+name+";",null,null).visitEnd();
        MethodVisitor init=writer.visitMethod(ACC_PUBLIC,"<init>","(L"+name+";Landroid/os/PermissionEnforcer;)V",null,null);init.visitCode();init.visitVarInsn(ALOAD,0);
        boolean enforcer=stub.methods.stream().anyMatch(method->method.name.equals("<init>")&&method.desc.equals("(Landroid/os/PermissionEnforcer;)V"));
        if(enforcer)init.visitVarInsn(ALOAD,2);
        init.visitMethodInsn(INVOKESPECIAL,stub.name,"<init>",enforcer?"(Landroid/os/PermissionEnforcer;)V":"()V",false);
        init.visitVarInsn(ALOAD,0);init.visitVarInsn(ALOAD,1);init.visitFieldInsn(PUTFIELD,generated,"target","L"+name+";");init.visitInsn(RETURN);init.visitMaxs(0,0);init.visitEnd();
        Map<String,MethodNode> methods=new TreeMap<>();methods(name,methods,new HashSet<>());
        for(var entry:methods.entrySet()){
            MethodNode method=entry.getValue();if(concrete(stub.name,entry.getKey()))continue;
            MethodVisitor code=writer.visitMethod(ACC_PUBLIC,method.name,method.desc,null,method.exceptions.toArray(String[]::new));code.visitCode();code.visitVarInsn(ALOAD,0);code.visitFieldInsn(GETFIELD,generated,"target","L"+name+";");
            int slot=1;for(Type argument:Type.getArgumentTypes(method.desc)){code.visitVarInsn(argument.getOpcode(ILOAD),slot);slot+=argument.getSize();}
            code.visitMethodInsn(INVOKEINTERFACE,name,method.name,method.desc,true);code.visitInsn(Type.getReturnType(method.desc).getOpcode(IRETURN));code.visitMaxs(0,0);code.visitEnd();
        }
        writer.visitEnd();Path file=output.resolve(generated+".class");Files.createDirectories(file.getParent());Files.write(file,writer.toByteArray());
        generateClient(name,methods,output);
    }
    private void generateClient(String name,Map<String,MethodNode> methods,Path output)throws IOException{
        String generated="maclauncher/android/framework/clients/"+name;
        String boundary="maclauncher/android/framework/HostServiceClients";
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_MAXS);
        writer.visit(V21,ACC_PUBLIC|ACC_FINAL,generated,null,"java/lang/Object",new String[]{name,boundary+"$Target"});
        writer.visitField(ACC_PRIVATE|ACC_FINAL,"target","L"+name+";",null,null).visitEnd();
        writer.visitField(ACC_PRIVATE|ACC_FINAL,"binder","Landroid/os/IBinder;",null,null).visitEnd();
        MethodVisitor init=writer.visitMethod(ACC_PUBLIC,"<init>","(L"+name+";Landroid/os/IBinder;)V",null,null);init.visitCode();
        init.visitVarInsn(ALOAD,0);init.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V",false);
        init.visitVarInsn(ALOAD,0);init.visitVarInsn(ALOAD,1);init.visitFieldInsn(PUTFIELD,generated,"target","L"+name+";");
        init.visitVarInsn(ALOAD,0);init.visitVarInsn(ALOAD,2);init.visitFieldInsn(PUTFIELD,generated,"binder","Landroid/os/IBinder;");init.visitInsn(RETURN);init.visitMaxs(0,0);init.visitEnd();
        MethodVisitor original=writer.visitMethod(ACC_PUBLIC,"platformTarget","()Landroid/os/IInterface;",null,null);original.visitCode();original.visitVarInsn(ALOAD,0);original.visitFieldInsn(GETFIELD,generated,"target","L"+name+";");original.visitInsn(ARETURN);original.visitMaxs(0,0);original.visitEnd();
        for(MethodNode method:methods.values()){
            MethodVisitor code=writer.visitMethod(ACC_PUBLIC,method.name,method.desc,null,method.exceptions.toArray(String[]::new));code.visitCode();
            if(method.name.equals("asBinder")&&method.desc.equals("()Landroid/os/IBinder;")){
                code.visitVarInsn(ALOAD,0);code.visitFieldInsn(GETFIELD,generated,"binder","Landroid/os/IBinder;");code.visitInsn(ARETURN);code.visitMaxs(0,0);code.visitEnd();continue;
            }
            Type[] arguments=Type.getArgumentTypes(method.desc);Type result=Type.getReturnType(method.desc);
            int identity=1;List<Object> locals=new ArrayList<>();locals.add(generated);
            for(Type argument:arguments){identity+=argument.getSize();locals.add(frameType(argument));}
            int value=identity+2,exception=value+result.getSize();
            code.visitMethodInsn(INVOKESTATIC,"android/os/Binder","clearCallingIdentity","()J",false);code.visitVarInsn(LSTORE,identity);
            Label start=new Label(),end=new Label(),failed=new Label();code.visitTryCatchBlock(start,end,failed,null);code.visitLabel(start);
            code.visitVarInsn(ALOAD,0);code.visitFieldInsn(GETFIELD,generated,"target","L"+name+";");
            int slot=1;for(Type argument:arguments){
                code.visitVarInsn(argument.getOpcode(ILOAD),slot);slot+=argument.getSize();
                if(argument.getSort()==Type.OBJECT){code.visitMethodInsn(INVOKESTATIC,boundary,"argument","(Ljava/lang/Object;)Ljava/lang/Object;",false);code.visitTypeInsn(CHECKCAST,argument.getInternalName());}
            }
            code.visitMethodInsn(INVOKEINTERFACE,name,method.name,method.desc,true);
            if(result.getSort()==Type.OBJECT||result.getSort()==Type.ARRAY){
                code.visitLdcInsn(result);code.visitMethodInsn(INVOKESTATIC,boundary,"result","(Ljava/lang/Object;Ljava/lang/Class;)Ljava/lang/Object;",false);code.visitTypeInsn(CHECKCAST,result.getInternalName());
            }
            if(result.getSort()!=Type.VOID)code.visitVarInsn(result.getOpcode(ISTORE),value);
            code.visitLabel(end);code.visitVarInsn(LLOAD,identity);code.visitMethodInsn(INVOKESTATIC,"android/os/Binder","restoreCallingIdentity","(J)V",false);
            if(result.getSort()!=Type.VOID)code.visitVarInsn(result.getOpcode(ILOAD),value);code.visitInsn(result.getOpcode(IRETURN));
            code.visitLabel(failed);locals.add(LONG);code.visitFrame(F_FULL,locals.size(),locals.toArray(),1,new Object[]{"java/lang/Throwable"});
            code.visitVarInsn(ASTORE,exception);code.visitVarInsn(LLOAD,identity);code.visitMethodInsn(INVOKESTATIC,"android/os/Binder","restoreCallingIdentity","(J)V",false);code.visitVarInsn(ALOAD,exception);code.visitInsn(ATHROW);code.visitMaxs(0,0);code.visitEnd();
        }
        writer.visitEnd();Path file=output.resolve(generated+".class");Files.createDirectories(file.getParent());Files.write(file,writer.toByteArray());
    }
    private static Object frameType(Type type){return switch(type.getSort()){
        case Type.BOOLEAN,Type.BYTE,Type.CHAR,Type.SHORT,Type.INT->INTEGER;
        case Type.FLOAT->FLOAT;case Type.LONG->LONG;case Type.DOUBLE->DOUBLE;default->type.getInternalName();
    };}
    public static void main(String[] args)throws Exception{
        ServiceEndpoints tool=new ServiceEndpoints(Path.of(args[0]));Path output=Path.of(args[1]);
        // Every supplied AIDL contract has its original Stub dispatcher. Service
        // discovery is not limited to interfaces referenced by current adapters.
        // Concrete supplied Binder implementations are still exported unchanged.
        try(tool.framework){
            for(var entry:tool.framework.stream().filter(entry->entry.getName().endsWith("$Stub.class")).sorted(Comparator.comparing(JarEntry::getName)).toList())
                tool.generate(entry.getName().substring(0,entry.getName().length()-"$Stub.class".length()),output);
        }
    }
}
