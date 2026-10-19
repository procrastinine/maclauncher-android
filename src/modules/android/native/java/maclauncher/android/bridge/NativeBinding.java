package maclauncher.android.bridge;
import java.lang.reflect.*;
import java.util.*;
/** Native image and binding lifetime matches the host JVM. */
public final class NativeBinding {
    /** One process owns one guest native directory; UTF-8 is the filesystem encoding. */
    static synchronized native void configureRuntime(byte[] runtime,byte[] nativeRoot);
    private static native void registerEntry(Class<?> entry);
    static synchronized native void prepareNativeActivity();
    static Method entry(ClassLoader guest){
        String name="maclauncher.android.bridge.GuestNativeEntry";
        try(var stream=NativeBinding.class.getResourceAsStream("/"+name.replace('.','/')+".class")){
            if(stream==null)throw new IllegalStateException("Missing native entry helper");
            byte[] bytes=stream.readAllBytes();
            class EntryLoader extends ClassLoader {
                EntryLoader(){super(guest);}
                Class<?> define(){return defineClass(name,bytes,0,bytes.length);}
            }
            Class<?> type=new EntryLoader().define();registerEntry(type);
            return type.getMethod("load",String.class,String.class,String.class,boolean.class);
        }catch(java.io.IOException|ReflectiveOperationException failure){throw new IllegalStateException("Native entry setup failed",failure);}
    }
    static synchronized long loadIn(Method method,String runtime,String file,String soname,boolean initialize){
        try{return (long)method.invoke(null,runtime,file,soname,initialize);}
        catch(InvocationTargetException failure){Throwable cause=failure.getCause();if(cause instanceof Error error)throw error;if(cause instanceof RuntimeException error)throw error;throw new IllegalStateException(cause);}
        catch(IllegalAccessException failure){throw new IllegalStateException(failure);}
    }
    static void constructIn(Method method,long image){invokeImage(method,"construct",image);}
    static void rejectIn(Method method,long image){invokeImage(method,"reject",image);}
    static void initializeIn(Method method,long image){invokeImage(method,"initialize",image);}
    private static void invokeImage(Method method,String operation,long image){
        try{method.getDeclaringClass().getMethod(operation,long.class).invoke(null,image);}
        catch(InvocationTargetException failure){Throwable cause=failure.getCause();if(cause instanceof Error error)throw error;if(cause instanceof RuntimeException error)throw error;throw new IllegalStateException(cause);}
        catch(ReflectiveOperationException failure){throw new IllegalStateException(failure);}
    }

    public static synchronized native long load(String runtime,String file,String soname);
    private static synchronized native int register(long image,Class<?> target,String[] names,String[] descriptors,String[] shortExports,String[] longExports);
    private static native String[] nativeMethods(Class<?> target);
    record NativeMethod(String name,String descriptor,boolean isStatic) {}
    static List<NativeMethod> inventory(Class<?> target){
        Objects.requireNonNull(target);
        if(target.isArray()||target.isPrimitive()||target.isInterface())throw new IllegalArgumentException("Native methods require a class");
        String[] fields=nativeMethods(target);
        if(fields==null){
            // A loaded class may not yet be linked. Resolve the inherited final
            // Object method without invoking it or running class initialization.
            try{java.lang.invoke.MethodHandles.privateLookupIn(target,java.lang.invoke.MethodHandles.lookup())
                .findVirtual(target,"getClass",java.lang.invoke.MethodType.methodType(Class.class));}
            catch(ReflectiveOperationException failure){
                if(failure.getCause() instanceof LinkageError error)throw error;
                throw new IllegalStateException("Native method class could not be prepared",failure);
            }
            fields=nativeMethods(target);
        }
        if(fields==null||fields.length%3!=0)throw new IllegalStateException("Invalid native method inventory");
        List<NativeMethod> result=new ArrayList<>(fields.length/3);
        for(int i=0;i<fields.length;i+=3){
            if(!"static".equals(fields[i+2])&&!"instance".equals(fields[i+2]))throw new IllegalStateException("Invalid native method modifiers");
            result.add(new NativeMethod(fields[i],fields[i+1],"static".equals(fields[i+2])));
        }
        result.sort(Comparator.comparing(NativeMethod::name).thenComparing(NativeMethod::descriptor));
        return List.copyOf(result);
    }
    static String mangle(String value){
        StringBuilder out=new StringBuilder();
        for(int i=0;i<value.length();i++){
            char c=value.charAt(i);
            if(c>='A'&&c<='Z'||c>='a'&&c<='z'||c>='0'&&c<='9')out.append(c);
            else switch(c){case '/'->out.append('_');case '_'->out.append("_1");case ';'->out.append("_2");case '['->out.append("_3");default->out.append(String.format("_0%04x",(int)c));}
        }
        return out.toString();
    }
    public static int bind(long image,Class<?> target){
        return bindAvailable(image,target,null);
    }
    /** Register this image's exported methods, including classes spanning libraries. */
    public static int bindAvailable(long image,Class<?> target,Set<String> exports){
        List<NativeMethod> methods=inventory(target).stream().filter(m->{
            if(exports==null)return true;
            String descriptor=m.descriptor();
            String shortName="Java_"+mangle(target.getName().replace('.','/'))+"_"+mangle(m.name());
            return exports.contains(shortName)||exports.contains(shortName+"__"+mangle(descriptor.substring(1,descriptor.indexOf(')'))));
        }).toList();
        String[] names=new String[methods.size()],descriptors=new String[names.length],shorts=new String[names.length],longs=new String[names.length];
        for(int i=0;i<names.length;i++){
            NativeMethod m=methods.get(i);names[i]=m.name();descriptors[i]=m.descriptor();
            shorts[i]="Java_"+mangle(target.getName().replace('.','/'))+"_"+mangle(names[i]);
            longs[i]=shorts[i]+"__"+mangle(descriptors[i].substring(1,descriptors[i].indexOf(')')));
        }
        if(names.length==0)return 0;
        int count=register(image,target,names,descriptors,shorts,longs);
        if(count!=names.length)throw new IllegalStateException("Incomplete JNI registration for "+target.getName());
        return count;
    }
}
