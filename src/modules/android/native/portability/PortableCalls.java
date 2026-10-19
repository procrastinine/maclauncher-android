package maclauncher.android.portability;

import java.util.*;
import org.objectweb.asm.*;
import org.objectweb.asm.commons.ClassRemapper;
import org.objectweb.asm.commons.Remapper;
import org.objectweb.asm.tree.*;

/** Shared exact JVM boundaries for the original framework and converted DEX. */
public final class PortableCalls {
    public static final String HELPER="maclauncher/android/framework/JvmPortability";
    public static final String CLEANER="maclauncher/android/framework/JvmCleaner",MEMORY="maclauncher/android/framework/JvmNativeMemory";
    public static final String REGISTRY="libcore/util/NativeAllocationRegistry";
    public final SortedMap<String,Long> counts=new TreeMap<>();
    private record Call(int opcode,String owner,String name,String descriptor,String category){}
    // Framework loadLibrary calls refer to the supplied platform modules. APK
    // calls retain the converter's guest native-loader routing instead.
    private final boolean platformLibraries;
    public PortableCalls(){this(true);}
    public PortableCalls(boolean platformLibraries){this.platformLibraries=platformLibraries;}
    private Call mapped(int opcode,String owner,String name,String descriptor){
        if(opcode==Opcodes.INVOKEVIRTUAL&&owner.equals("java/security/spec/ECParameterSpec")&&((name.equals("getCurveName")&&descriptor.equals("()Ljava/lang/String;"))||(name.equals("setCurveName")&&descriptor.equals("(Ljava/lang/String;)V"))))
            return new Call(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostEcParameters",name,"(Ljava/security/spec/ECParameterSpec;"+descriptor.substring(1),"ec-parameter-metadata");
        if(opcode==Opcodes.INVOKESTATIC&&owner.equals("java/nio/NioUtils"))
            return new Call(opcode,"maclauncher/android/framework/HostNio",name,descriptor,"jdk-nio");
        if(opcode==Opcodes.INVOKEVIRTUAL&&owner.equals("java/util/zip/ZipEntry")&&name.equals("getDataOffset")&&descriptor.equals("()J"))
            return new Call(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostStrictJarFile","dataOffset","(Ljava/util/zip/ZipEntry;)J","zip-entry-offset");
        if(opcode==Opcodes.INVOKEVIRTUAL&&(owner.equals("java/net/Socket")||owner.equals("java/net/DatagramSocket"))&&name.equals("getFileDescriptor$")&&descriptor.equals("()Ljava/io/FileDescriptor;"))
            return new Call(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostFileDescriptor","socketDescriptor","(L"+owner+";)Ljava/io/FileDescriptor;","socket-descriptor");
        if(opcode==Opcodes.INVOKEVIRTUAL&&owner.equals("java/io/FileDescriptor")){
            String adapted=switch(name+descriptor){
                case "getInt$()I"->"getInt";case "setInt$(I)V"->"setInt";
                case "isSocket$()Z"->"isSocket";case "getOwnerId$()J"->"getOwnerId";
                case "setOwnerId$(J)V"->"setOwnerId";case "release$()Ljava/io/FileDescriptor;"->"release";
                case "cloneForFork()V"->"cloneForFork";default->null;
            };
            if(adapted!=null)return new Call(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostFileDescriptor",adapted,"(Ljava/io/FileDescriptor;"+descriptor.substring(1),"file-descriptor");
        }
        if(opcode==Opcodes.INVOKESTATIC&&owner.equals(REGISTRY)&&descriptor.equals("(J)V")){
            if(name.equals("registerNativeAllocation"))return new Call(opcode,MEMORY,"registerAllocation",descriptor,"native-memory");
            if(name.equals("registerNativeFree"))return new Call(opcode,MEMORY,"registerFree",descriptor,"native-memory");
        }
        if(opcode==Opcodes.INVOKEVIRTUAL&&owner.equals("dalvik/system/VMRuntime")&&name.equals("newUnpaddedArray")&&descriptor.equals("(Ljava/lang/Class;I)Ljava/lang/Object;"))
            return new Call(Opcodes.INVOKESTATIC,HELPER,name,"(Ljava/lang/Object;Ljava/lang/Class;I)Ljava/lang/Object;","array-allocation");
        if(opcode==Opcodes.INVOKEVIRTUAL&&owner.equals("java/util/LinkedHashMap")&&name.equals("eldest")&&descriptor.equals("()Ljava/util/Map$Entry;"))
            return new Call(Opcodes.INVOKESTATIC,HELPER,name,"(Ljava/util/LinkedHashMap;)Ljava/util/Map$Entry;","collections");
        if(opcode==Opcodes.INVOKESTATIC&&owner.equals("java/util/Locale")&&name.equals("adjustLanguageCode")&&descriptor.equals("(Ljava/lang/String;)Ljava/lang/String;"))
            return new Call(opcode,HELPER,name,descriptor,"locale");
        if(opcode==Opcodes.INVOKESTATIC&&owner.equals("java/lang/System")){
            if(name.equals("getenv")&&(descriptor.equals("(Ljava/lang/String;)Ljava/lang/String;")||descriptor.equals("()Ljava/util/Map;")))
                return new Call(opcode,"maclauncher/android/framework/HostFilesystem","environment",descriptor,"android-environment");
            if(platformLibraries&&name.equals("loadLibrary")&&descriptor.equals("(Ljava/lang/String;)V"))
                return new Call(opcode,"maclauncher/android/framework/HostFrameworkLibraries",name,descriptor,"platform-library");
            if(name.equals("arraycopy")&&!descriptor.equals("(Ljava/lang/Object;ILjava/lang/Object;II)V")){
                Type[] arguments=Type.getArgumentTypes(descriptor);
                if(arguments.length==5&&Type.getReturnType(descriptor).equals(Type.VOID_TYPE)
                    &&arguments[0].getSort()==Type.ARRAY&&arguments[2].getSort()==Type.ARRAY
                    &&arguments[1].equals(Type.INT_TYPE)&&arguments[3].equals(Type.INT_TYPE)&&arguments[4].equals(Type.INT_TYPE))
                    return new Call(opcode,owner,name,"(Ljava/lang/Object;ILjava/lang/Object;II)V","array-copy");
            }
            if((name.equals("logE")||name.equals("logW"))&&(descriptor.equals("(Ljava/lang/String;)V")||descriptor.equals("(Ljava/lang/String;Ljava/lang/Throwable;)V")))
                return new Call(opcode,HELPER,name,descriptor,"diagnostic-logging");
        }
        return null;
    }
    public byte[] normalize(byte[] original){
        original=installationIdentity(original);
        original=socketPlatform(original);
        original=NioPortability.transform(original);
        ClassReader reader=new ClassReader(original);ClassWriter writer=new ClassWriter(reader,0);int[] changed={0};
        // Keep the original ART-specific wrapper and every one of its signatures.
        // Only its callers use the JDK cleanup owner; changing its constructor
        // descriptor would hide an upstream method.
        if(reader.getClassName().equals(REGISTRY+"$CleanerRunner"))return original;
        boolean[] initialize={reader.getClassName().equals(REGISTRY)};
        var normalizedLoaders=new java.util.HashSet<String>();
        if(initialize[0])reader.accept(new ClassVisitor(Opcodes.ASM9){
            @Override public MethodVisitor visitMethod(int access,String name,String desc,String signature,String[] exceptions){
                if(!name.equals("<clinit>")&&!name.equals("<init>"))return null;
                return new MethodVisitor(Opcodes.ASM9){@Override public void visitMethodInsn(int opcode,String owner,String method,String descriptor,boolean iface){
                    if(opcode==Opcodes.INVOKESTATIC&&owner.equals(MEMORY)&&method.equals("initialize")&&descriptor.equals("()V"))initialize[0]=false;
                    if(opcode==Opcodes.INVOKESTATIC&&owner.equals(MEMORY)&&method.equals("allocationLoader")&&descriptor.equals("(Ljava/lang/ClassLoader;)Ljava/lang/ClassLoader;"))normalizedLoaders.add(name+desc);
                }};
            }
        },ClassReader.SKIP_DEBUG|ClassReader.SKIP_FRAMES);
        var remapper=new Remapper(Opcodes.ASM9){@Override public String map(String type){
            if(type.equals(REGISTRY+"$CleanerRunner")){changed[0]++;counts.merge("cleaner:runner",1L,Long::sum);return CLEANER+"$Runner";}
            if(type.equals("sun/misc/Cleaner")){changed[0]++;counts.merge("cleaner:type",1L,Long::sum);return CLEANER;}return type;
        }};
        reader.accept(new ClassVisitor(Opcodes.ASM9,new ClassRemapper(writer,remapper)){
            @Override public MethodVisitor visitMethod(int access,String name,String desc,String signature,String[] exceptions){
                MethodVisitor visitor=super.visitMethod(access,name,desc,signature,exceptions);
                if((access&(Opcodes.ACC_NATIVE|Opcodes.ACC_ABSTRACT))!=0)return visitor;
                return new MethodVisitor(Opcodes.ASM9,visitor){
                    @Override public void visitCode(){
                        super.visitCode();
                        if(reader.getClassName().equals(REGISTRY)&&name.equals("<init>")&&desc.startsWith("(Ljava/lang/ClassLoader;")&&!normalizedLoaders.contains(name+desc)){
                            changed[0]++;counts.merge("native-memory:bootstrap-loader",1L,Long::sum);
                            super.visitVarInsn(Opcodes.ALOAD,1);
                            super.visitMethodInsn(Opcodes.INVOKESTATIC,MEMORY,"allocationLoader","(Ljava/lang/ClassLoader;)Ljava/lang/ClassLoader;",false);
                            super.visitVarInsn(Opcodes.ASTORE,1);
                        }
                    }
                    @Override public void visitInsn(int opcode){
                        if(opcode==Opcodes.RETURN&&initialize[0]&&name.equals("<clinit>")&&desc.equals("()V")){
                            changed[0]++;counts.merge("native-memory:lazy-initialize",1L,Long::sum);
                            super.visitMethodInsn(Opcodes.INVOKESTATIC,MEMORY,"initialize","()V",false);
                        }
                        super.visitInsn(opcode);
                    }
                    @Override public void visitMethodInsn(int opcode,String owner,String method,String descriptor,boolean isInterface){
                        Call mapped=mapped(opcode,owner,method,descriptor);
                        if(mapped==null){super.visitMethodInsn(opcode,owner,method,descriptor,isInterface);return;}
                        changed[0]++;counts.merge(mapped.category+":"+owner+"."+method+descriptor,1L,Long::sum);
                        super.visitMethodInsn(mapped.opcode,mapped.owner,mapped.name,mapped.descriptor,false);
                    }
                };
            }
        },0);
        return changed[0]==0?original:writer.toByteArray();
    }
    /** Preserve the provider's original SSAID checks, aliases and persisted
     * tables while retaining this installation's existing random identity. */
    private static byte[] installationIdentity(byte[] original){
        ClassReader reader=new ClassReader(original);
        if(!reader.getClassName().equals("com/android/providers/settings/SettingsProvider"))return original;
        ClassWriter writer=new ClassWriter(reader,0);
        reader.accept(new ClassVisitor(Opcodes.ASM9,writer){
            @Override public MethodVisitor visitMethod(int access,String name,String descriptor,String signature,String[] exceptions){
                MethodVisitor visitor=super.visitMethod(access,name,descriptor,signature,exceptions);
                return new MethodVisitor(Opcodes.ASM9,visitor){
                    @Override public void visitMethodInsn(int opcode,String owner,String method,String desc,boolean iface){
                        if(name.equals("getSsaidSettingLocked")&&owner.equals("android/os/Binder")&&((method.equals("clearCallingIdentity")&&desc.equals("()J"))||(method.equals("restoreCallingIdentity")&&desc.equals("(J)V")))){
                            super.visitMethodInsn(opcode,"maclauncher/android/framework/HostAndroidIdentity",method,desc,false);
                        }else if(owner.equals("android/content/pm/IPackageManager")&&method.equals("getInstantAppAndroidId")&&desc.equals("(Ljava/lang/String;I)Ljava/lang/String;")){
                            super.visitMethodInsn(Opcodes.INVOKESTATIC,"maclauncher/android/framework/HostAndroidIdentity","applicationId","(Landroid/content/pm/IPackageManager;Ljava/lang/String;I)Ljava/lang/String;",false);
                        }else super.visitMethodInsn(opcode,owner,method,desc,iface);
                    }
                };
            }
        },0);
        return writer.toByteArray();
    }

    private static byte[] socketPlatform(byte[] original){
        var reader=new ClassReader(original);
        if(!reader.getClassName().equals("com/android/org/conscrypt/Platform"))return original;
        var type=new ClassNode(Opcodes.ASM9);reader.accept(type,0);
        for(var method:type.methods)if(method.name.equals("getFileDescriptorFromSSLSocket")&&method.desc.equals("(Lcom/android/org/conscrypt/AbstractConscryptSocket;)Ljava/io/FileDescriptor;")){
            // This is the original Conscrypt OpenJDK platform implementation:
            // delegate to the same socket descriptor boundary, without ART's
            // reflective access to private java.net implementation fields.
            method.instructions.clear();method.tryCatchBlocks.clear();method.localVariables=null;
            method.visibleLocalVariableAnnotations=null;method.invisibleLocalVariableAnnotations=null;
            method.instructions.add(new VarInsnNode(Opcodes.ALOAD,0));
            method.instructions.add(new MethodInsnNode(Opcodes.INVOKESTATIC,type.name,"getFileDescriptor","(Ljava/net/Socket;)Ljava/io/FileDescriptor;",false));
            method.instructions.add(new InsnNode(Opcodes.ARETURN));
        }
        var writer=new ClassWriter(reader,ClassWriter.COMPUTE_MAXS);type.accept(writer);return writer.toByteArray();
    }
}
