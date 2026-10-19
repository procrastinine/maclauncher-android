package maclauncher.android.bridge;
import java.util.Objects;

/** Dynamic modules enter the shared native loader with their actual defining class. */
public final class DynamicGuestNativeLoader {
    private static final StackWalker CALLER=StackWalker.getInstance(StackWalker.Option.RETAIN_CLASS_REFERENCE);
    private DynamicGuestNativeLoader(){}
    public static void load(String path){GuestNativeLoader.loadFrom(CALLER.getCallerClass(),path);}
    public static void loadLibrary(String name){GuestNativeLoader.loadLibraryFrom(CALLER.getCallerClass(),name);}
    public static void runtimeLoad(Runtime runtime,String path){Objects.requireNonNull(runtime);GuestNativeLoader.loadFrom(CALLER.getCallerClass(),path);}
    public static void runtimeLoadLibrary(Runtime runtime,String name){Objects.requireNonNull(runtime);GuestNativeLoader.loadLibraryFrom(CALLER.getCallerClass(),name);}
    public static String mapLibraryName(String name){return GuestNativeLoader.mapLibraryName(name);}
    public static String getProperty(String name){return GuestNativeLoader.getProperty(name);}
    public static String getProperty(String name,String fallback){return GuestNativeLoader.getProperty(name,fallback);}
}
