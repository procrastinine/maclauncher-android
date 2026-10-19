package maclauncher.android;

import java.nio.file.Path;
import maclauncher.android.framework.*;

/** Common native bootstrap for an APK process and the profile platform process. */
final class RuntimeBootstrap {
    private RuntimeBootstrap() {}
    static void initialize(Path runtime,Path data,Path files,Path cache,String scope,
                           boolean networkAllowed,boolean allowAds,boolean graphics)throws Exception {
        HostStartupTiming.mark("bootstrap-start");
        System.setProperty("java.awt.headless","true");
        HostDigests.initialize();
        HostFilesystem.configure(runtime,data,files,cache,scope);
        HostStartupTiming.mark("bootstrap-filesystem");
        if(!scope.equals("android")&&!scope.equals("android.keystore"))HostAndroidIdentity.prepare(data);
        System.setProperty("klepton.bridge",runtime.resolve("native/libklepton-jvm.dylib").toString());
        System.setProperty("klepton.graphics",runtime.resolve("native/libklepton-graphics.dylib").toString());
        System.setProperty("klepton.audio",runtime.resolve("native/libklepton-audio.dylib").toString());
        System.setProperty("klepton.graphics.allowGpu",Boolean.toString(graphics));
        System.load(System.getProperty("klepton.bridge"));
        HostProcessIdentity.initializeIds();
        HostStartupTiming.mark("bootstrap-bridge");
        System.load(runtime.resolve("native/libnativebinding.dylib").toString());
        HostStartupTiming.mark("bootstrap-nativebinding");
        JvmNativeMemory.configure(runtime.resolve("native/libandroid_runtime.dylib"));
        JvmNativeMemory.initialize();
        HostStartupTiming.mark("bootstrap-framework-native");
        maclauncher.android.net.HostNetworkPolicy.configure(allowAds);
        HostConnectivityManager.configure(networkAllowed);
        HostStartupTiming.mark("bootstrap-network");
    }
}
