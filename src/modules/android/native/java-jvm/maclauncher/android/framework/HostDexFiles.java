package maclauncher.android.framework;

import dalvik.system.BaseDexClassLoader;
import dalvik.system.DexFile;
import dalvik.system.PathClassLoader;
import java.io.*;
import java.lang.reflect.*;
import java.nio.ByteBuffer;
import java.nio.file.Path;
import java.util.*;
import maclauncher.android.DynamicDex;

/** ART DEX handles backed by the existing converter and real JVM defining loaders.
 * Android retains path ordering, shared-library delegation, resources and errors. */
public final class HostDexFiles {
    private HostDexFiles(){}
    /** ART's VMClassLoader accepts internal names and array descriptors. Keep
     * the same VM representation at every original DEX-loader entry point;
     * ordinary classes still use the complete upstream delegation algorithm. */
    public static String className(String name){return name.replace('/','.');}
    public static Class<?> arrayClass(String name,ClassLoader loader)throws ClassNotFoundException{
        return name.startsWith("[")?Class.forName(name,false,loader):null;
    }
    // Android's boot class path includes the framework supplied by the host.
    // This loader defines no classes and uses the same VM name boundary as the
    // original DEX-loader hierarchy; it never changes a defining loader.
    private static final class PlatformParent extends ClassLoader {
        static { registerAsParallelCapable(); }
        PlatformParent(){super(HostDexFiles.class.getClassLoader());}
        @Override protected Class<?> loadClass(String name,boolean resolve)throws ClassNotFoundException{
            name=className(name);Class<?> array=arrayClass(name,this);
            return array!=null?array:super.loadClass(name,resolve);
        }
    }
    /** APK resources precede resources in the host's combined framework archive.
     * Class definitions and the original DEX/shared-library ordering are unchanged. */
    private static final class ApplicationLoader extends PathClassLoader {
        static { registerAsParallelCapable(); }
        ApplicationLoader(String libraries,ClassLoader parent){super("",libraries,parent);}
        @Override public java.net.URL getResource(String name){
            java.net.URL resource=findResource(Objects.requireNonNull(name));
            return resource!=null?resource:getParent().getResource(name);
        }
        @Override public Enumeration<java.net.URL> getResources(String name)throws IOException{
            Enumeration<java.net.URL> resources=findResources(Objects.requireNonNull(name));
            Enumeration<java.net.URL> inherited=getParent().getResources(name);
            return new Enumeration<>() {
                @Override public boolean hasMoreElements(){return resources.hasMoreElements()||inherited.hasMoreElements();}
                @Override public java.net.URL nextElement(){return resources.hasMoreElements()?resources.nextElement():inherited.nextElement();}
            };
        }
    }
    private static final ClassLoader platformParent=new PlatformParent();
    private record ApplicationSources(Path classes,List<Path> apks,boolean demand){}
    private static final Map<ClassLoader,ApplicationSources> applications=new WeakHashMap<>();
    private record PreparedSource(Path classes,Set<String> members,Set<String> dependencies,List<Path> apks,boolean demand){}
    private static volatile Map<Path,PreparedSource> installations=Map.of();
    /** Prepared code is shared as immutable bytes, never as a defining loader.
     * Membership comes from the converter's original DEX parser, so each AOSP
     * path element retains precisely its APK's classes and resource ordering. */
    public static synchronized void installed(Path converted,List<Path> sources)throws IOException{
        Path classes=converted.toRealPath();var files=new LinkedHashMap<String,Path>();
        for(Path source:sources){Path file=source.toRealPath();files.put(file.getFileName().toString(),file);}
        boolean demand=DynamicDex.isDemand(classes);var apks=List.copyOf(files.values());
        var next=new HashMap<>(installations);
        try(var jar=new java.util.jar.JarFile(classes.toFile())){
            Set<String> dependencies=null;
            var dependencyEntry=jar.getJarEntry("META-INF/maclauncher/parent-classes");
            if(dependencyEntry!=null)try(var input=new DataInputStream(jar.getInputStream(dependencyEntry))){
                if(input.readInt()==1){
                    int count=input.readInt();var names=new LinkedHashSet<String>();
                    for(int i=0;i<count;i++)names.add(input.readUTF());
                    if(count>=0&&input.read()==-1)dependencies=Set.copyOf(names);
                }
            }
            var entry=jar.getJarEntry("META-INF/maclauncher/apk-classes");
            if(entry==null){
                // Older single-APK preparations have the same unambiguous
                // membership. Split archives without provenance use DEX normally.
                if(files.size()==1)next.put(files.values().iterator().next(),new PreparedSource(classes,null,dependencies,apks,demand));
            }else try(var input=new DataInputStream(jar.getInputStream(entry))){
                if(input.readInt()!=1)return;
                int count=input.readInt();if(count!=files.size())return;
                var found=new LinkedHashMap<Path,PreparedSource>();boolean independent=true;
                for(int i=0;i<count;i++){
                    Path file=files.get(input.readUTF());boolean unique=input.readBoolean();int length=input.readInt();
                    if(file==null||length<0)return;var members=new LinkedHashSet<String>();
                    for(int j=0;j<length;j++)members.add(input.readUTF());
                    independent&=unique;found.put(file,new PreparedSource(classes,Set.copyOf(members),dependencies,apks,demand));
                }
                // A combined conversion keeps the first duplicate definition.
                // Arbitrary split ordering needs the original per-APK converter.
                if(independent&&found.size()==files.size()&&input.read()==-1)next.putAll(found);
            }
        }
        installations=Map.copyOf(next);
    }
    public static void clearInstalled(){installations=Map.of();}
    /** This derived lookup follows the canonical package row's source lease.
     * Existing defining loaders retain their own DEX modules independently. */
    public static synchronized void uninstalled(List<Path> sources){
        var next=new HashMap<>(installations);for(Path source:sources)next.remove(source);
        installations=Map.copyOf(next);
    }
    private static Class<?> resolve(ClassLoader loader,String name){
        try{return Class.forName(className(name),false,loader);}
        catch(ClassNotFoundException|LinkageError unavailable){return null;}
    }
    private static boolean preparedContext(ClassLoader loader,PreparedSource source){
        if(loader==null)return false;
        if(source.members()!=null&&source.members().isEmpty())return true;
        ClassLoader parent=parent(loader.getParent());
        var shared=new ArrayList<ClassLoader>();
        try{if(loader instanceof BaseDexClassLoader)for(Field field:List.of(Fields.before,Fields.after)){ClassLoader[] values=(ClassLoader[])field.get(loader);if(values!=null)Collections.addAll(shared,values);}}
        catch(IllegalAccessException failure){throw new IllegalStateException("Android DEX ownership is unavailable",failure);}
        // Older preparations have no dependency provenance. Their original
        // platform-only context remains valid without changing installed files.
        if(source.dependencies()==null)return parent==platformParent&&shared.isEmpty();
        // Preserve the original loader, including wrappers and shared libraries.
        // Only the external types actually consulted during conversion matter;
        // different resolution uses the existing dynamic converter normally.
        for(String name:source.dependencies()){
            Class<?> actual=resolve(parent,name);
            if(actual==null)for(ClassLoader library:shared)if((actual=resolve(library,name))!=null)break;
            if(actual!=resolve(platformParent,name))return false;
        }
        return true;
    }
    /** Original PathClassLoader owns delegation, native paths and APK resources. */
    public static PathClassLoader application(Path converted,List<Path> sourceApks,Path nativeRoot,ClassLoader parent)throws IOException{
        Path classes=converted.toRealPath();var apks=new LinkedHashSet<Path>();
        for(Path source:sourceApks)apks.add(source.toRealPath());
        PathClassLoader loader=new ApplicationLoader(nativeRoot.toRealPath().toString(),parent(parent));
        synchronized(applications){applications.put(loader,new ApplicationSources(classes,List.copyOf(apks),DynamicDex.isDemand(classes)));}
        var paths=new ArrayList<String>();paths.add(classes.toString());apks.forEach(file->paths.add(file.toString()));
        loader.addDexPath(String.join(File.pathSeparator,paths));return loader;
    }
    private static final class OpenDex implements AutoCloseable {
        final DynamicDex.Module module;
        OpenDex(DynamicDex.Module module){this.module=module;}
        public void close(){module.run();}
    }
    private static Object cookie(DynamicDex.Module module){
        try{return new long[]{0,HostNativeHandle.create(new OpenDex(module))};}
        catch(RuntimeException|Error failure){module.run();throw failure;}
    }
    private static DynamicDex.Module module(Object cookie){
        if(!(cookie instanceof long[] handles)||handles.length!=2)throw new IllegalStateException("Android DEX file is closed");
        synchronized(handles){if(handles[1]==0)throw new IllegalStateException("Android DEX file is closed");return ((OpenDex)HostNativeHandle.state(handles[1])).module;}
    }
    public static ClassLoader parent(ClassLoader supplied){
        // ApplicationLoaders derives Android's boot parent from the system
        // loader's parent. HotSpot puts only JDK modules there; Android's full
        // framework lives with this host. Normalize that exact platform boundary
        // while preserving every caller-supplied delegating loader unchanged.
        return supplied==null||supplied==ClassLoader.getPlatformClassLoader()
            ||supplied==HostDexFiles.class.getClassLoader()?platformParent:supplied;
    }
    public static ClassLoader bootClassLoader(Class<?> ignored){return platformParent;}
    public static Object openDexFileNative(String source,String output,int flags,ClassLoader loader,Object[] elements)throws IOException{
        ApplicationSources application;synchronized(applications){application=applications.get(loader);}
        if(application!=null){Path file=Path.of(source).toRealPath();
            if(file.equals(application.classes()))return cookie(application.demand()?DynamicDex.demand(file,application.apks(),parent(loader.getParent()),null):DynamicDex.prepared(file));
            // The prepared archive includes all APK DEX inputs; these original
            // path elements retain their resource ZIPs without reconverting code.
            if(application.apks().contains(file))return cookie(DynamicDex.prepared(null));
        }
        PreparedSource prepared=installations.get(Path.of(source).toRealPath());
        if(prepared!=null&&preparedContext(loader,prepared))return cookie(prepared.demand()?DynamicDex.demand(prepared.classes(),prepared.apks(),parent(loader.getParent()),prepared.members()):DynamicDex.prepared(prepared.classes(),prepared.members()));
        try{return cookie(DynamicDex.paths(source,parent(loader==null?null:loader.getParent())));}
        catch(IllegalArgumentException failure){throw new IOException(failure.getMessage(),failure);}
    }
    public static Object openInMemoryDexFilesNative(ByteBuffer[] buffers,byte[][] arrays,int[] starts,int[] ends,ClassLoader loader,Object[] elements)throws IOException{
        try{return cookie(DynamicDex.buffers(buffers,parent(loader==null?null:loader.getParent())));}
        catch(IllegalArgumentException failure){throw new IOException(failure.getMessage(),failure);}
    }
    public static boolean closeDexFile(Object cookie){
        if(cookie==null)return true;
        if(!(cookie instanceof long[] handles)||handles.length!=2)throw new IllegalArgumentException("Invalid Android DEX handle");
        long handle;synchronized(handles){handle=handles[1];handles[1]=0;}if(handle!=0)HostNativeHandle.release(handle);return true;
    }
    public static Class<?> defineClassNative(String name,ClassLoader loader,Object cookie,DexFile owner)throws ClassNotFoundException{
        try{byte[] bytes=module(cookie).classBytes(name.replace('/','.'));if(bytes==null)return null;Class<?> type=defineBytecode(name.replace('.','/'),Objects.requireNonNull(loader),bytes);if(type!=null)maclauncher.android.bridge.GuestNativeLoader.classDefined(type);return type;}
        catch(IOException failure){throw new ClassNotFoundException(name,failure);}
    }
    private static native Class<?> defineBytecode(String name,ClassLoader loader,byte[] bytes);
    public static String[] getClassNameList(Object cookie){return module(cookie).classNames();}
    public static boolean isBackedByOatFile(Object cookie){module(cookie);return false;}
    // HotSpot verifies every class during definition. ART background/OAT verification
    // and ART trusted-loader flags do not disable that JVM verification.
    public static void verifyInBackgroundNative(Object cookie,ClassLoader loader){module(cookie);}
    public static void setTrusted(Object cookie){module(cookie);}
    public static boolean isReadOnlyJavaDclEnforced(){return false;}
    public static String[] computeClassLoaderContextsNative(BaseDexClassLoader loader){return new String[0];}

    private static final class Fields {
        static final Field path=field(BaseDexClassLoader.class,"pathList");
        static final Field elements=field(path.getType(),"dexElements");
        static final Field dex=field(elements.getType().componentType(),"dexFile");
        static final Field cookie=field(DexFile.class,"mCookie");
        static final Field before=field(BaseDexClassLoader.class,"sharedLibraryLoaders"),after=field(BaseDexClassLoader.class,"sharedLibraryLoadersAfter");
    }
    private static Field field(Class<?> type,String name){try{Field value=type.getDeclaredField(name);value.setAccessible(true);return value;}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    /** Converter headers follow the original loader's actual DEX elements. */
    public static List<Path> headers(BaseDexClassLoader loader){
        var result=new LinkedHashSet<Path>();headers(loader,result,Collections.newSetFromMap(new IdentityHashMap<>()));return List.copyOf(result);
    }
    private static void headers(BaseDexClassLoader loader,Set<Path> result,Set<ClassLoader> visited){
        if(!visited.add(loader))return;
        try{
            for(Field field:List.of(Fields.before,Fields.after)){ClassLoader[] shared=(ClassLoader[])field.get(loader);if(shared!=null)for(ClassLoader at:shared)if(at instanceof BaseDexClassLoader dex)headers(dex,result,visited);}
            Object path=Fields.path.get(loader);if(path==null)return;Object[] elements=(Object[])Fields.elements.get(path);if(elements==null)return;
            for(Object element:elements){if(element==null)continue;Object dex=Fields.dex.get(element);if(dex==null)continue;Object cookie=Fields.cookie.get(dex);if(cookie!=null)result.addAll(module(cookie).headers());}
        }catch(IllegalAccessException failure){throw new IllegalStateException("Android DEX ownership is unavailable",failure);}
    }
}
