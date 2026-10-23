package maclauncher.android.bridge;
import java.io.IOException;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.zip.*;

/** Android native loads from converted APK callsites and the guest JNI facade. */
public final class GuestNativeLoader {
    private static final StackWalker CALLER=StackWalker.getInstance(StackWalker.Option.RETAIN_CLASS_REFERENCE);
    private static volatile State state;
    private static final class Owner {
        final ClassLoader loader;final Set<String> classes=new LinkedHashSet<>();
        private final java.util.concurrent.atomic.AtomicReference<java.lang.reflect.Method> nativeEntry=new java.util.concurrent.atomic.AtomicReference<>();
        Owner(ClassLoader loader,Collection<String> classes){this.loader=loader;this.classes.addAll(classes);}
        java.lang.reflect.Method entry(){
            var ready=nativeEntry.get();if(ready!=null)return ready;
            // Class definition enters the guest loader and may reenter classDefined.
            // Never hold a registry/owner monitor or wait for another defining thread.
            var candidate=NativeBinding.entry(loader);
            return nativeEntry.compareAndSet(null,candidate)?candidate:nativeEntry.get();
        }
    }
    private static final class Image {
        volatile Thread linkingThread=Thread.currentThread();
        final CompletableFuture<Void> linked=new CompletableFuture<>(),constructed=new CompletableFuture<>();
        final Map<Class<?>,CompletableFuture<Void>> bindings=new IdentityHashMap<>();
        final Set<Owner> users=Collections.newSetFromMap(new IdentityHashMap<>());
        volatile long handle;Set<String> exports=Set.of();int registered;
        Owner initializingOwner;Thread initializingThread;CompletableFuture<Void> initialized;
    }
    private static final class State {
        final String runtime;final Path nativeRoot;final Owner application;
        final Map<ClassLoader,Owner> owners=new IdentityHashMap<>();
        final Map<ClassLoader,Set<String>> dynamicClasses=new WeakHashMap<>();
        final Map<Path,Image> loaded=new HashMap<>();
        State(String runtime,Path nativeRoot,ClassLoader loader,List<String> classes){this.runtime=runtime;this.nativeRoot=nativeRoot;application=new Owner(loader,classes);owners.put(loader,application);}
    }
    public static synchronized void configure(String runtime,String nativeRoot,String jar,ClassLoader loader)throws IOException {
        if(state!=null)throw new IllegalStateException("Guest native loader already configured");
        Path root=Path.of(nativeRoot).toRealPath();if(!Files.isDirectory(root))throw new IOException("APK native directory is missing");
        List<String> classes=new ArrayList<>();
        try(ZipFile archive=new ZipFile(jar)){
            for(var entries=archive.entries();entries.hasMoreElements();){
                ZipEntry entry=entries.nextElement();String name=entry.getName();
                if(!name.endsWith(".class")||name.startsWith("META-INF/"))continue;
                classes.add(name.substring(0,name.length()-6).replace('/','.'));
            }
        }
        if(classes.isEmpty())classes.addAll(Arrays.asList(maclauncher.android.DynamicDex.preparedClassNames(Path.of(jar))));
        State candidate=new State(Path.of(runtime).toRealPath().toString(),root,Objects.requireNonNull(loader),List.copyOf(classes));
        NativeBinding.configureRuntime(candidate.runtime.getBytes(java.nio.charset.StandardCharsets.UTF_8),root.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));state=candidate;
    }
    public static void runtimeLoad(Runtime receiver,String filename){Objects.requireNonNull(receiver);loadFrom(CALLER.getCallerClass(),filename);}
    public static void runtimeLoadLibrary(Runtime receiver,String name){Objects.requireNonNull(receiver);loadLibraryFrom(CALLER.getCallerClass(),name);}
    public static String mapLibraryName(String name){return "lib"+Objects.requireNonNull(name)+".so";}
    public static String nativeLibraryPath(){return requireState().nativeRoot.toString();}
    public static String findLibrary(String name){
        if(name==null||name.isEmpty()||name.indexOf('/')>=0||name.indexOf('\\')>=0||name.indexOf('\0')>=0)return null;
        State value=requireState();Path path=value.nativeRoot.resolve(mapLibraryName(name));
        try{Path real=path.toRealPath();return real.startsWith(value.nativeRoot)&&Files.isRegularFile(real)?real.toString():null;}catch(IOException missing){return null;}
    }
    private static Path libraryPath(ClassLoader loader,String name){
        Objects.requireNonNull(name);
        if(name.isEmpty()||name.indexOf('/')>=0||name.indexOf('\\')>=0||name.indexOf('\0')>=0)throw new UnsatisfiedLinkError("Invalid Android native library name");
        String filename=loader instanceof dalvik.system.BaseDexClassLoader dex?dex.findLibrary(name):null;
        return checkedFile(filename==null?requireState().nativeRoot.resolve(mapLibraryName(name)).toString():filename);
    }
    private static Path checkedFile(String filename){
        Objects.requireNonNull(filename);State value=requireState();Path requested=Path.of(filename);
        if(!requested.isAbsolute())throw new UnsatisfiedLinkError("Android native library path must be absolute");
        try{Path file=requested.toRealPath();
            if(!(file.startsWith(value.nativeRoot)||file.startsWith(maclauncher.android.framework.HostContext.hostDataRoot())||maclauncher.android.framework.HostPackages.hostNativeCode(file))||!Files.isRegularFile(file))throw new UnsatisfiedLinkError("Native library is outside the installed application directories");
            return file;
        }catch(IOException failure){throw failure("Android native library could not load: "+requested.getFileName(),failure);}
    }
    public static void loadLibrary(String name){loadLibraryFrom(CALLER.getCallerClass(),name);}
    public static void load(String filename){loadFrom(CALLER.getCallerClass(),filename);}
    /** JNI calls enter JDK System/Runtime methods without converted bytecode.
     * Keep the actual Java native declaration as caller; a native-only thread
     * borrows its image's real declaration and defining class-loader namespace. */
    public static void loadFromNative(Class<?> anchor,String name,boolean library){
        Class<?> caller=CALLER.walk(frames->frames.map(StackWalker.StackFrame::getDeclaringClass)
            .filter(type->type!=GuestNativeLoader.class).findFirst().orElse(anchor));
        State value=requireState();Owner nativeOwner=null;
        synchronized(value){
            for(Owner candidate:value.owners.values()){
                var entry=candidate.nativeEntry.get();
                if(entry!=null&&(entry.getDeclaringClass()==caller||caller==null&&entry.getDeclaringClass()==anchor)){nativeOwner=candidate;break;}
            }
        }
        if(nativeOwner==null){if(caller==null)throw new UnsatisfiedLinkError("Native load has no Android application class loader");nativeOwner=owner(caller);}
        Path file=library?libraryPath(nativeOwner.loader,name):checkedFile(name);load(nativeOwner,file,true);
    }
    static void loadFrom(Class<?> caller,String filename){Path file=checkedFile(filename);load(owner(caller),file,true);}
    static void loadLibraryFrom(Class<?> caller,String name){Path file=libraryPath(caller.getClassLoader(),name);load(owner(caller),file,true);}
    private static Owner owner(Class<?> caller){
        Objects.requireNonNull(caller);State value=requireState();ClassLoader loader=caller.getClassLoader();
        if(loader==null||loader==GuestNativeLoader.class.getClassLoader())throw new UnsatisfiedLinkError("Native load has no Android application class loader");
        synchronized(value){Owner owner=value.owners.get(loader);
            if(owner==null){owner=new Owner(loader,value.dynamicClasses.getOrDefault(loader,Set.of()));value.owners.put(loader,owner);value.dynamicClasses.remove(loader);}
            owner.classes.add(caller.getName());return owner;
        }
    }
    /** Defined classes retain their actual loader; Java-only modules remain weakly indexed. */
    public static void classDefined(Class<?> type){
        State value=state;if(value==null)return;ClassLoader loader=type.getClassLoader();Owner owner;List<Image> images;
        synchronized(value){owner=value.owners.get(loader);if(owner==null){value.dynamicClasses.computeIfAbsent(loader,ignored->new LinkedHashSet<>()).add(type.getName());return;}owner.classes.add(type.getName());images=List.copyOf(value.loaded.values());}
        for(Image image:images){boolean available;synchronized(image){available=image.handle!=0&&image.users.contains(owner)&&(image.initializingOwner==null||image.initializingOwner==owner);}if(available)bind(image,type,owner);}
    }
    private static void bind(Image image,Class<?> type,Owner owner){
        if(type.getClassLoader()!=owner.loader)return;
        String prefix="Java_"+NativeBinding.mangle(type.getName().replace('.','/'))+"_";
        if(image.exports.stream().noneMatch(symbol->symbol.startsWith(prefix)))return;
        CompletableFuture<Void> completion;boolean execute;
        synchronized(image){completion=image.bindings.get(type);execute=completion==null;if(execute){completion=new CompletableFuture<>();image.bindings.put(type,completion);}}
        if(!execute){await(completion);return;}
        try{int count=NativeBinding.bindAvailable(image.handle,type,image.exports);synchronized(image){image.registered+=count;}completion.complete(null);}
        catch(Throwable failure){completion.completeExceptionally(failure);throw propagate(failure);}
    }
    private static void bind(Image image,Owner owner)throws ClassNotFoundException{
        List<String> classes;State value=requireState();synchronized(value){classes=List.copyOf(owner.classes);}
        for(String name:classes){String prefix="Java_"+NativeBinding.mangle(name.replace('.','/'))+"_";
            if(image.exports.stream().anyMatch(symbol->symbol.startsWith(prefix)))bind(image,Class.forName(name,false,owner.loader),owner);}
    }
    private static void initialize(Image image,Owner owner,Path file){
        CompletableFuture<Void> completion;boolean execute;
        synchronized(image){
            if(image.initializingOwner!=null&&image.initializingOwner!=owner)throw new UnsatisfiedLinkError("Android native library is already owned by another class loader: "+file.getFileName());
            completion=image.initialized;execute=completion==null;
            if(execute){image.initializingOwner=owner;image.initializingThread=Thread.currentThread();completion=new CompletableFuture<>();image.initialized=completion;}
            else if(!completion.isDone()&&image.initializingThread==Thread.currentThread())return;
        }
        if(!execute){await(completion);return;}
        try{bind(image,owner);NativeBinding.initializeIn(owner.entry(),image.handle);completion.complete(null);}
        catch(Throwable cause){var error=failure("Android JNI initialization failed: "+file.getFileName(),cause);completion.completeExceptionally(error);throw error;}
        finally{synchronized(image){image.initializingThread=null;}}
    }
    private static void load(Owner owner,Path file,boolean initialize){
        var nativeEntry=owner.entry();State value=requireState();Image image;boolean link;
        synchronized(value){image=value.loaded.get(file);link=image==null;
            if(link){image=new Image();value.loaded.put(file,image);}
            synchronized(image){
                if(initialize){if(image.initializingOwner!=null&&image.initializingOwner!=owner)throw new UnsatisfiedLinkError("Android native library is already owned by another class loader: "+file.getFileName());image.initializingOwner=owner;}
                image.users.add(owner);
            }
        }
        if(link){
            var preparationCount=new java.util.concurrent.atomic.AtomicInteger();
            var preparationFailure=new java.util.concurrent.atomic.AtomicReference<Throwable>();
            try{
                var metadata=ElfNativeExports.readMetadata(file);
                for(String dependency:metadata.dependencies()){Path sibling=file.getParent().resolve(dependency);if(!Files.exists(sibling))sibling=value.nativeRoot.resolve(dependency);if(Files.exists(sibling))load(owner,checkedFile(sibling.toString()),false);}
                Image preparedImage=image;
                long handle=NativeBinding.loadPreparedIn(nativeEntry,value.runtime,file.toString(),file.getFileName().toString(),prepared->{
                    try{
                        if(preparationCount.incrementAndGet()!=1)throw new UnsatisfiedLinkError("Android native preparation ran more than once");
                        if(prepared==0)throw new UnsatisfiedLinkError("Android native preparation returned no image");
                        synchronized(preparedImage){preparedImage.exports=metadata.exports();preparedImage.handle=prepared;}
                        preparedImage.linked.complete(null);bind(preparedImage,owner);
                    }catch(Throwable cause){preparationFailure.compareAndSet(null,cause);throw propagate(cause);}
                });
                if(preparationFailure.get()!=null)throw propagate(preparationFailure.get());
                if(preparationCount.get()!=1)throw new UnsatisfiedLinkError("Android native loader omitted preparation");
                if(handle==0||handle!=image.handle)throw new UnsatisfiedLinkError("Android native prepared image does not match its loaded image");
                // The native entry runs constructors after successful Java publication.
                // No registry, image or NativeBinding monitor spans that operation.
                image.constructed.complete(null);
            }catch(Throwable cause){Throwable first=preparationFailure.get();if(first!=null&&first!=cause){if(cause.getCause()!=first)first.addSuppressed(cause);cause=first;}if(image.handle!=0)try{NativeBinding.rejectIn(nativeEntry,image.handle);}catch(Throwable cleanup){if(cleanup!=cause)cause.addSuppressed(cleanup);}var error=failure("Android native library could not load: "+file.getFileName(),cause);image.linked.completeExceptionally(error);image.constructed.completeExceptionally(error);throw error;}
            finally{image.linkingThread=null;}
        }else{
            if(!image.linked.isDone()&&image.linkingThread==Thread.currentThread())throw new UnsatisfiedLinkError("Recursive Android native dependency: "+file.getFileName());
            await(image.linked);
            if(!image.constructed.isDone()&&image.linkingThread==Thread.currentThread())return;
            await(image.constructed);
        }
        if(initialize)initialize(image,owner,file);else try{bind(image,owner);}catch(ClassNotFoundException cause){throw failure("Android native class could not load",cause);}
    }
    private static void await(CompletableFuture<Void> future){try{future.join();}catch(CompletionException error){throw propagate(error.getCause());}}
    private static RuntimeException propagate(Throwable failure){if(failure instanceof Error error)throw error;if(failure instanceof RuntimeException error)return error;return new IllegalStateException(failure);}
    private static UnsatisfiedLinkError failure(String message,Throwable cause){var error=new UnsatisfiedLinkError(message+": "+cause.getMessage());error.initCause(cause);return error;}
    public static String getProperty(String name){
        Objects.requireNonNull(name);if(name.isEmpty())throw new IllegalArgumentException("Empty property name");
        return switch(name){
            case "java.vm.name" -> "Dalvik";
            // Legacy Android multidex parsers require an unqualified numeric version.
            // All original DEX inputs are already present in the converted classpath.
            case "java.vm.version" -> Runtime.version().feature()+"."+Runtime.version().interim()+"."+Runtime.version().update();
            case "java.runtime.name" -> "Android Runtime";
            case "java.vendor" -> "The Android Project";
            case "os.name" -> "Linux";
            case "os.arch" -> Set.of("armeabi-v7a","armeabi").contains(requireState().nativeRoot.getFileName().toString())?"armv7l":"aarch64";
            case "java.library.path" -> requireState().nativeRoot.toString();
            default -> System.getProperty(name);
        };
    }
    public static String getProperty(String name,String fallback){String value=getProperty(name);return value==null?fallback:value;}
    public static Map<String,Integer> loadedLibraries(){
        Map<String,Integer> result=new TreeMap<>();State value=requireState();synchronized(value){value.loaded.forEach((p,v)->{synchronized(v){if(v.handle!=0)result.put(p.getFileName().toString(),v.registered);}});}return Map.copyOf(result);
    }
    /** Reuses the same registered ELF image and guest-parented JNI entry. */
    public static ActivityHandle createActivity(String library,String entry,android.app.NativeActivity activity,byte[][] paths,int sdk,byte[] savedState){
        final long image;final Class<?> bridge;
        {
            if(library==null||!library.matches("[A-Za-z0-9_+.-]+")||entry==null||!entry.matches("[A-Za-z_][A-Za-z0-9_]*"))throw new IllegalArgumentException("Invalid NativeActivity library or entry point");
            State s=requireState();NativeBinding.prepareNativeActivity();
            Path file=s.nativeRoot.resolve(mapLibraryName(library));
            Owner owner=activity.getClass().getClassLoader()==GuestNativeLoader.class.getClassLoader()?s.application:owner(activity.getClass());load(owner,checkedFile(file.toString()),false); // dlopen is not a Java System.loadLibrary call.
            synchronized(s){image=s.loaded.get(checkedFile(file.toString())).handle;}
            bridge=owner.entry().getDeclaringClass();
        }
        // Guest callbacks may load another library or wait for a guest worker.
        // Never hold either loader monitor across those calls.
        try{
            var create=bridge.getMethod("createActivity",long.class,String.class,android.app.NativeActivity.class,byte[][].class,int.class,byte[].class);
            var event=bridge.getMethod("activityEvent",long.class,int.class,long.class);
            long handle=(long)invoke(create,image,entry,activity,paths,sdk,savedState);
            if(handle==0)throw new IllegalStateException("NativeActivity returned no owner");
            return new ActivityHandle(handle,event,bridge.getMethod("activityContentRect",long.class,int.class,int.class,int.class,int.class));
        }catch(ReflectiveOperationException error){throw new IllegalStateException("NativeActivity entry is unavailable",error);}
    }
    private static Object invoke(java.lang.reflect.Method method,Object... arguments){
        try{return method.invoke(null,arguments);}
        catch(java.lang.reflect.InvocationTargetException failure){Throwable cause=failure.getCause();if(cause instanceof Error error)throw error;if(cause instanceof RuntimeException error)throw error;throw new IllegalStateException(cause);}
        catch(IllegalAccessException failure){throw new IllegalStateException(failure);}
    }
    public static final class ActivityHandle {
        private long handle;private final java.lang.reflect.Method event,contentRect;private final Thread owner=Thread.currentThread();
        private ActivityHandle(long handle,java.lang.reflect.Method event,java.lang.reflect.Method contentRect){this.handle=handle;this.event=event;this.contentRect=contentRect;}
        public void contentRect(int x,int y,int width,int height){if(Thread.currentThread()!=owner||handle==0)throw new IllegalStateException("NativeActivity callbacks require their live owner");invoke(contentRect,handle,x,y,width,height);}
        public byte[] event(int kind,long value){
            if(Thread.currentThread()!=owner)throw new IllegalStateException("NativeActivity callbacks require the Activity thread");
            long current=handle;if(current==0)throw new IllegalStateException("NativeActivity is destroyed");
            if(kind==6)handle=0;
            return (byte[])invoke(event,current,kind,value);
        }
    }
    private static State requireState(){if(state==null)throw new IllegalStateException("Guest native loader is not configured");return state;}
}
