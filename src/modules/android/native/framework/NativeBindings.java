import java.io.*;
import java.util.*;
import java.nio.file.*;
import org.objectweb.asm.*;
import org.objectweb.asm.tree.*;

/** Bind native boundaries from compiled platform declarations, never name lists.
 * Original Java methods and native entries without an exact adapter stay intact. */
final class NativeBindings implements Opcodes {
    private record Adapter(String owner,boolean receiver){}
    private static Adapter framework(String owner){return new Adapter("maclauncher/android/framework/"+owner,true);}
    private static Adapter media(String owner){return new Adapter("maclauncher/android/media/"+owner,true);}
    // Actual native implementations, not framework eligibility: every unmatched
    // class and every unmatched method passes through unchanged.
    private static final Map<String,Adapter> owners=Map.ofEntries(
        Map.entry("android/app/NativeActivity",framework("HostNativeActivity")),
        Map.entry("android/view/InputQueue",framework("HostInputQueue")),
        Map.entry("android/view/MotionEvent",framework("HostMotionEvent")),
        Map.entry("android/view/KeyCharacterMap",framework("HostKeyCharacterMap")),
        Map.entry("android/window/InputTransferToken",framework("HostInputTransferToken")),
        Map.entry("android/view/Surface",framework("HostSurface")),
        Map.entry("android/hardware/HardwareBuffer",framework("HostHardwareBuffer")),
        Map.entry("android/hardware/SystemSensorManager",framework("HostSensors")),
        Map.entry("android/hardware/SystemSensorManager$BaseEventQueue",framework("HostSensors")),
        Map.entry("android/graphics/GraphicBuffer",framework("HostHardwareBuffer")),
        Map.entry("android/view/SurfaceControl",framework("HostSurfaceControl")),
        Map.entry("android/view/SurfaceSession",framework("HostSurfaceSession")),
        Map.entry("android/graphics/BLASTBufferQueue",framework("HostBufferQueue")),
        Map.entry("android/graphics/HardwareRenderer",framework("HostPixelCopy")),
        Map.entry("android/view/DisplayEventReceiver",framework("HostDisplayEvents")),
        Map.entry("android/os/incremental/IncrementalManager",framework("HostIncrementalFilesystem")),
        Map.entry("android/util/jar/StrictJarFile",framework("HostStrictJarFile")),
        Map.entry("dalvik/system/VMDebug",framework("HostVmDebug")),
        Map.entry("dalvik/system/DexFile",framework("HostDexFiles")),
        Map.entry("dalvik/system/BaseDexClassLoader",framework("HostDexFiles")),
        Map.entry("android/os/FileObserver$ObserverThread",framework("HostFileObserver")),
        Map.entry("libcore/io/Linux",new Adapter("maclauncher/android/framework/HostFileDescriptor",false)),
        Map.entry("android/net/TrafficStats",framework("HostTrafficStats")),
        Map.entry("android/os/ServiceManager",framework("HostServices")),
        Map.entry("android/media/SoundPool",media("HostSoundPool")),
        Map.entry("android/media/MediaPlayer",media("HostMediaPlayer")),
        Map.entry("android/media/AudioRecord",media("HostAudioRecord")),
        Map.entry("android/media/AudioTrack",media("HostAudioTrack")),
        Map.entry("android/media/AudioSystem",media("HostAudioSystem")),
        Map.entry("android/media/MediaMetrics",media("HostMediaMetrics")),
        Map.entry("android/media/audiopolicy/AudioProductStrategy",media("HostAudioSystem")),
        Map.entry("android/media/audiopolicy/AudioVolumeGroup",media("HostAudioSystem")),
        Map.entry("android/media/AudioPortEventHandler",media("HostAudioPortEventHandler")),
        Map.entry("android/hardware/Camera",media("HostCamera"))
    );
    static byte[] transform(byte[] original){Adapter adapter=owners.get(new ClassReader(original).getClassName());return adapter==null?original:transform(original,adapter.owner(),adapter.receiver());}
    private static final Map<String,Set<String>> declarations=new HashMap<>();
    private static Set<String> methods(String owner) {
        return declarations.computeIfAbsent(owner,key->{
            try(InputStream input=NativeBindings.class.getClassLoader().getResourceAsStream(key+".class")){
                if(input==null)throw new IllegalStateException("Compiled Android platform owner is missing: "+key);
                Set<String> result=new HashSet<>();
                new ClassReader(input).accept(new ClassVisitor(ASM9){
                    @Override public MethodVisitor visitMethod(int access,String name,String descriptor,String signature,String[] exceptions){
                        if((access&(ACC_PUBLIC|ACC_STATIC|ACC_ABSTRACT))==(ACC_PUBLIC|ACC_STATIC))result.add(name+descriptor);
                        return null;
                    }
                },ClassReader.SKIP_CODE|ClassReader.SKIP_DEBUG|ClassReader.SKIP_FRAMES);
                return Set.copyOf(result);
            }catch(IOException failure){throw new UncheckedIOException(failure);}
        });
    }
    /** The framework transform reads these compiled declarations, never host bodies. */
    public static void main(String[] args)throws IOException {
        if(args.length!=1)throw new IllegalArgumentException("Expected compiled binding metadata output");
        StringBuilder output=new StringBuilder();
        for(String owner:owners.values().stream().map(Adapter::owner).distinct().sorted().toList())
            for(String method:methods(owner).stream().sorted().toList())output.append(owner).append('\t').append(method).append('\n');
        Files.writeString(Path.of(args[0]),output);
    }
    private static boolean accepts(Type source,Type target){
        if(source.equals(target))return true;
        boolean reference=source.getSort()==Type.OBJECT||source.getSort()==Type.ARRAY;
        if(reference&&target.getDescriptor().equals("Ljava/lang/Object;"))return true;
        // Package-private components cannot appear in external Java source.
        // Preserve native descriptors; only the adapter accepts a wider type.
        return source.getSort()==Type.ARRAY&&target.getDescriptor().equals("[Ljava/lang/Object;")
            &&(source.getDimensions()>1||source.getElementType().getSort()==Type.OBJECT);
    }
    private static String binding(Set<String> available,String name,String descriptor){
        if(available.contains(name+descriptor))return descriptor;
        String selected=null;Type[] source=Type.getArgumentTypes(descriptor);Type result=Type.getReturnType(descriptor);
        for(String declaration:available){
            if(!declaration.startsWith(name+"("))continue;
            String candidate=declaration.substring(name.length());Type[] target=Type.getArgumentTypes(candidate);
            if(source.length!=target.length||!result.equals(Type.getReturnType(candidate)))continue;
            boolean matches=true;for(int i=0;i<source.length;i++)if(!accepts(source[i],target[i])){matches=false;break;}
            if(!matches)continue;
            if(selected!=null)throw new IllegalStateException("Ambiguous native platform binding: "+name+descriptor);
            selected=candidate;
        }
        return selected;
    }
    static byte[] transform(byte[] original,String platformOwner) {
        return transform(original,platformOwner,true);
    }
    private static byte[] transform(byte[] original,String platformOwner,boolean receiver) {
        Set<String> available=methods(platformOwner);
        ClassNode node=new ClassNode();new ClassReader(original).accept(node,0);
        boolean changed=false;
        for(MethodNode method:node.methods){
            if((method.access&ACC_NATIVE)==0)continue;
            boolean instance=(method.access&ACC_STATIC)==0;
            String descriptor=instance&&receiver?"(L"+node.name+";"+method.desc.substring(1):method.desc;
            descriptor=binding(available,method.name,descriptor);
            if(descriptor==null)continue;
            method.access&=~ACC_NATIVE;
            if(instance&&receiver)method.instructions.add(new VarInsnNode(ALOAD,0));
            int slot=instance?1:0;
            for(Type argument:Type.getArgumentTypes(method.desc)){
                method.instructions.add(new VarInsnNode(argument.getOpcode(ILOAD),slot));slot+=argument.getSize();
            }
            method.instructions.add(new MethodInsnNode(INVOKESTATIC,platformOwner,method.name,descriptor,false));
            method.instructions.add(new InsnNode(Type.getReturnType(method.desc).getOpcode(IRETURN)));
            changed=true;
        }
        if(!changed)return original;
        ClassWriter output=new ClassWriter(ClassWriter.COMPUTE_MAXS);node.accept(output);return output.toByteArray();
    }
}
