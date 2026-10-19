package maclauncher.android.framework;

import java.nio.file.Path;
import java.util.Objects;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** AOSP native cleanup with the Java 21 direct-memory budget and cleaner owner. */
public final class JvmNativeMemory {
    private static String library;
    private static boolean initialized,registered;
    private static Throwable initializationFailure;
    private static boolean registering;
    private static final java.util.Set<Class<?>> pendingInitializers=new java.util.LinkedHashSet<>();
    public static synchronized boolean isRegistered(){return registered;}
    public static ClassLoader allocationLoader(ClassLoader loader){return loader==null?JvmNativeMemory.class.getClassLoader():loader;}
    private JvmNativeMemory() {}
    /** JVM class initialization can precede RegisterNatives. Only classes actually
     * reached during registration defer their complete upstream initializer. */
    public static synchronized void initializeClass(Class<?> type){
        if(registering&&!registered){pendingInitializers.add(type);return;}
        if(!registered)initialize();
        try{var initializer=type.getDeclaredMethod("hostInitializeNatives");initializer.setAccessible(true);initializer.invoke(null);}
        catch(java.lang.reflect.InvocationTargetException failure){Throwable cause=failure.getCause();if(cause instanceof RuntimeException error)throw error;if(cause instanceof Error error)throw error;throw new IllegalStateException("Android native class initialization failed: "+type.getName(),cause);}
        catch(ReflectiveOperationException failure){throw new IllegalStateException("Android native class initializer is unavailable: "+type.getName(),failure);}
    }
    static synchronized String nativeLibrary(){if(!registered)throw new IllegalStateException("Android native framework is not initialized");return library;}
    /** Read-only installed component root, also available before an app Context exists. */
    public static synchronized Path runtimeDirectory(){if(library==null)throw new IllegalStateException("Android native framework is not configured");return Path.of(library).getParent().getParent();}

    /** Called before guest initialization with the validated runtime payload path. */
    public static synchronized void configure(Path path) {
        String value=Objects.requireNonNull(path).toAbsolutePath().normalize().toString();
        if(library!=null&&!library.equals(value))throw new IllegalStateException("Android native framework is already configured");
        library=value;
    }

    /** Entered by the actual NativeAllocationRegistry class initializer, on demand. */
    public static synchronized void initialize() {
        if(initializationFailure!=null)throw new IllegalStateException("Android native framework initialization failed",initializationFailure);
        if(initialized||registering)return;
        if(library==null)throw new IllegalStateException("Android native framework is not configured");
        // Initialize the registry's real bookkeeping before graphics registration can allocate.
        registering=true;
        try {
        Class.forName("libcore.util.NativeAllocationRegistry",true,JvmNativeMemory.class.getClassLoader());
        initializeBudget();
        Path icu=Path.of(library).resolveSibling("icudt76l.dat");
        if(!java.nio.file.Files.isRegularFile(icu))throw new IllegalStateException("Android native ICU data is unavailable");
        System.setProperty("icu.data.path",icu.toString());
        System.setProperty("icu.locale.default",java.util.Locale.getDefault().toLanguageTag());
        String[] names={"use_base_native_hostruntime","core_native_classes","graphics_native_classes","method_binding_format"};
        java.util.Properties families=new java.util.Properties();
        try(var stream=JvmNativeMemory.class.getResourceAsStream("native-families.properties")){
            if(stream==null)throw new IllegalStateException("Android native registration catalog is unavailable");
            families.load(stream);
        }catch(java.io.IOException failure){throw new IllegalStateException("Android native registration catalog could not be read",failure);}
        String[] values={"true",families.getProperty("core"),families.getProperty("graphics"),"${method}"};
        String[] previous=new String[names.length];
        for(int i=0;i<names.length;i++)previous[i]=System.getProperty(names[i]);
        try {
            for(int i=0;i<names.length;i++)System.setProperty(names[i],values[i]);
            prepareRegistration(library.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            System.load(library);
            completeRegistration();
            registered=true;
            System.load(Path.of(library).resolveSibling("libandroid-binder-rpc.dylib").toString());
            Runtime.getRuntime().addShutdownHook(new Thread(JvmNativeMemory::retireRuntime,"Android native shutdown"));
            System.load(Path.of(library).resolveSibling("libandroid-libcore.dylib").toString());
            System.load(Path.of(library).resolveSibling("libandroid-media.dylib").toString());
            System.load(Path.of(library).resolveSibling("libandroid-ndk-graphics.dylib").toString());
            configureResourceDirectory(runtimeDirectory().resolve("android-root/system/usr/hyphen-data").toString());
            while(!pendingInitializers.isEmpty()){Class<?> type=pendingInitializers.iterator().next();pendingInitializers.remove(type);initializeClass(type);}
            HostCrypto.initialize();
            HostSystemFonts.initialize(null);
            // Use the original application publication path: it retains the
            // shared font map and registers every collection with Minikin.
            android.graphics.Typeface.setSystemFontMap(android.graphics.Typeface.serializeFontMap(
                    android.graphics.Typeface.getSystemFontMap()));
            android.text.Hyphenator.init();
            initialized=true;
        } finally {
            for(int i=0;i<names.length;i++)if(previous[i]==null)System.clearProperty(names[i]);else System.setProperty(names[i],previous[i]);
        }
        }catch(RuntimeException|Error failure){initializationFailure=failure;throw failure;}
        catch(ClassNotFoundException|java.io.IOException|android.system.ErrnoException failure){initializationFailure=failure;throw new IllegalStateException(failure);}
        finally{registering=false;}
    }

    // AOSP encodes IS_MALLOCED in bit zero of its nonnegative size estimate.
    // Both allocation kinds share the real JVM budget; cleanup balances both.
    public static void registerAllocation(long encodedSize) { reserve(bytes(encodedSize)); }
    public static void registerFree(long encodedSize) { unreserve(bytes(encodedSize)); }
    /** Binder's native-reference pressure requests collection asynchronously in
     * ART. Keep that boundary asynchronous on HotSpot too: a Binder caller may
     * hold locks that reference cleanup needs. Overlapping requests coalesce,
     * and the single daemon worker retires when idle. */
    public static void requestCollection() {
        if(!Collector.pending.compareAndSet(false,true))return;
        try {
            Collector.worker.execute(()->{
                try { System.gc(); }
                finally { Collector.pending.set(false); }
            });
        } catch(RuntimeException|Error failure) { Collector.pending.set(false);throw failure; }
    }
    private static final class Collector {
        static final AtomicBoolean pending=new AtomicBoolean();
        static final ThreadPoolExecutor worker=new ThreadPoolExecutor(0,1,5,TimeUnit.SECONDS,
                new LinkedBlockingQueue<>(1),Thread.ofPlatform().daemon(true).name("Android requested GC").factory());
    }
    private static long bytes(long encodedSize) {
        if(encodedSize<0)throw new IllegalArgumentException("Negative native allocation size");
        return encodedSize&~1L;
    }
    private static native void initializeBudget();
    private static native void retireRuntime();
    private static native void prepareRegistration(byte[] library);
    private static native void completeRegistration();
    private static native void configureResourceDirectory(String hyphenDirectory);
    private static native void reserve(long bytes);
    private static native void unreserve(long bytes);
}
