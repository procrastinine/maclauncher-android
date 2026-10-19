package maclauncher.android.framework;

import java.nio.file.Path;
import java.util.Objects;
import maclauncher.android.media.AudioHost;

/** Android module names resolve to the actual supplied Mac implementations. */
public final class HostFrameworkLibraries {
    private HostFrameworkLibraries(){}
    public static void loadLibrary(String name){
        switch(Objects.requireNonNull(name)){
            case "android_runtime" -> JvmNativeMemory.initialize();
            case "media_jni" -> {JvmNativeMemory.initialize();AudioHost.initialize();}
            case "soundpool" -> AudioHost.initialize();
            case "javacrypto" -> System.load(Path.of(JvmNativeMemory.nativeLibrary()).resolveSibling("libjavacrypto.dylib").toString());
            case "alarm_jni" -> System.load(Path.of(JvmNativeMemory.nativeLibrary()).resolveSibling("libalarm_jni.dylib").toString());
            case "framework-connectivity-tiramisu-jni" -> {
                // Both native socket-tag operations are implemented by this
                // compiled Java platform owner; there is no separate Android SO.
                try{Class.forName(HostTrafficStats.class.getName(),true,HostFrameworkLibraries.class.getClassLoader());}
                catch(ClassNotFoundException impossible){throw new ExceptionInInitializerError(impossible);}
            }
            default -> System.loadLibrary(name);
        }
    }
    public static void loadAudio(){
        String library=System.getProperty("klepton.audio");
        if(library==null){JvmNativeMemory.initialize();library=Path.of(JvmNativeMemory.nativeLibrary()).resolveSibling("libklepton-audio.dylib").toString();}
        System.load(library);
    }
}
