package maclauncher.android.framework;

import android.content.Context;
import android.app.Application;
import android.os.Looper;
import java.util.Objects;
import java.util.List;
import java.nio.file.Path;
import maclauncher.android.media.HostAudioManager;

/** Concrete macOS devices and local Binder services beneath the unchanged ContextImpl. */
public final class HostContextServices {
    private HostContextServices(){}
    public static void configureInputs(maclauncher.android.ApplicationSettings settings){
        HostInput.initialize();
        HostInput.configureTouchJoystick(settings.touchJoystick(),settings.joystickX(),settings.joystickY(),settings.joystickRadius());
        HostSensors.configure(settings.manualTilt());
    }
    public static void configureMedia(maclauncher.android.ApplicationSettings settings){
        maclauncher.android.media.MediaInputs.configure(settings.audioInputUid(),settings.cameraInputUid());
    }
    public static void configureBrowser(Path files,boolean networkAllowed,boolean webLinksAllowed,Path providerSource){
        maclauncher.android.bridge.WebLinks.hostConfigure(webLinksAllowed);
        maclauncher.android.web.WebKitFactory.configure(files.resolve("webview"),networkAllowed,providerSource);
    }
    /** Native representation capabilities; service policy and lifetime stay shared. */
    @FunctionalInterface public interface SensorPublication { AutoCloseable open(); }
    public record PhysicalBindings(boolean originalHardwareRenderer,HostAudioManager audio,SensorPublication sensors,List<Path> compatibilityDirectories) {
        public PhysicalBindings { Objects.requireNonNull(audio);if(compatibilityDirectories!=null)compatibilityDirectories=List.copyOf(compatibilityDirectories); }
        public PhysicalBindings(boolean originalHardwareRenderer,HostAudioManager audio,SensorPublication sensors){this(originalHardwareRenderer,audio,sensors,null);}
        public PhysicalBindings(boolean originalHardwareRenderer,HostAudioManager audio){this(originalHardwareRenderer,audio,null);}
    }
    static void install(Context context){install(context,new PhysicalBindings(false,new HostAudioManager()));}
    public static HostWindowService install(Context context,PhysicalBindings physical){
        Objects.requireNonNull(physical);
        HostPackages.install(context);
        HostConnectivityManager.install(context);
        HostPermissions.install();
        HostPermissions.installChecker(context);
        HostIdentity.install();
        HostPowerService.install(context);
        HostAlarmService.install(context);
        HostWindowService windows=HostWindowManager.install(context,physical.originalHardwareRenderer());
        maclauncher.android.media.HostAudioService.install(physical.audio(),context);
        HostAppOps.install(context);
        HostTrafficStats.install(context);
        HostClipboardService.install(context);
        HostUserService.install(context);
        HostUiModeService.install(context);
        HostNotificationService.install(context);
        maclauncher.android.location.HostLocationService.install(context);
        HostActivityTasks.install(context);
        com.android.server.compat.HostPlatformCompat.install(context,physical.compatibilityDirectories());
        HostAccounts.prepare(context);
        if(physical.sensors()!=null){
            AutoCloseable publication=Objects.requireNonNull(physical.sensors().open());
            Runnable close=()->{try{publication.close();}catch(RuntimeException|Error failure){throw failure;}catch(Exception failure){throw new IllegalStateException("Sensor publication could not close",failure);}};
            if(!HostLooper.addShutdownListener(context.getMainLooper(),close)){close.run();throw new IllegalStateException("Application Looper is closed");}
        }
        HostRemoteServices.publishPlatformServices(context);
        return windows;
    }
    public static void applicationCreated(Application application,Looper looper){HostJobScheduler.hostConfigure(application,looper);}
    /** Provider publication and its verified preference lease have the same order in either VM. */
    public static AutoCloseable installProviders(Application application,List<android.content.pm.ProviderInfo> inspected,
            maclauncher.android.LocalProviders providers,boolean acquire)throws Exception{
        AutoCloseable lease=HostGoogleProviders.libraryOperation(application,inspected,acquire);
        try{providers.install(application,inspected);return lease;}
        catch(Exception|Error failure){if(lease!=null)try{lease.close();}catch(Throwable cleanup){if(cleanup!=failure)failure.addSuppressed(cleanup);}throw failure;}
    }
    public static void applicationClosed(){try{HostJobScheduler.hostShutdown();}finally{HostSensors.shutdown();}}
    private static Throwable retire(Throwable first,Runnable action){try{action.run();}catch(Throwable failure){if(first==null)return failure;if(first!=failure)first.addSuppressed(failure);}return first;}
    /** Renderer owners drain their surfaces first; app/service owners then retire in one order. */
    public static Throwable retireApplication(Throwable first,maclauncher.android.LocalProviders providers){
        first=retire(first,maclauncher.android.bridge.WebLinks::hostShutdown);
        first=retire(first,maclauncher.android.media.MediaInputs::hostShutdown);
        first=retire(first,HostAsyncTasks::shutdown);
        first=retire(first,HostContextServices::applicationClosed);
        // Original service/context cleanup may still resolve classes, resources and providers.
        first=retire(first,HostApplication::shutdown);
        if(providers!=null)first=retire(first,providers::close);
        first=retire(first,maclauncher.android.bridge.LocalBroadcasts::shutdown);
        first=retire(first,HostConnectivityManager::hostClose);
        first=retire(first,maclauncher.android.location.ManualLocation::hostShutdown);
        first=retire(first,HostPackages::hostClearProviders);
        return retire(first,HostActivityManager::hostClearExitHistory);
    }
    /** Native queue disposal remains VM-owned; shared callbacks and Context state do not. */
    public static Throwable retireContext(Throwable first,Looper looper){
        return retireContext(first,looper,null);
    }
    public static Throwable retireContext(Throwable first,Looper looper,AutoCloseable loader){
        first=retire(first,()->HostLooper.retireCallbacks(looper));
        first=retire(first,FrameworkResources::shutdown);
        if(loader!=null)try{loader.close();}catch(Throwable failure){if(first==null)first=failure;else if(first!=failure)first.addSuppressed(failure);}
        first=retire(first,HostStrictMode::hostClearThreadPolicy);
        first=retire(first,android.net.TrafficStats::clearThreadStatsTag);
        return retire(first,HostContext::hostShutdown);
    }
}
