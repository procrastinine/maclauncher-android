package maclauncher.android.framework;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import com.android.server.DropBoxManagerService;
import com.android.server.SystemService;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;

/** The profile's system process owns original Android services and their files.
 * APK processes only receive original Binder clients; they cannot map this storage. */
public final class HostSystemServices implements AutoCloseable {
    /** Complete published endpoints of this owner, without constructing any
     * upstream service. Local ActivityManager remains process-owned; explicit
     * shared-directory queries use the same system process key separately. */
    public static Map<String,String> declarations(){return Map.ofEntries(
        Map.entry(android.content.Context.DROPBOX_SERVICE,com.android.internal.os.IDropBoxManagerService.class.getName()),
        Map.entry("statsmanager",android.os.IStatsManagerService.class.getName()),
        Map.entry("statscompanion",android.os.IStatsCompanionService.class.getName()),
        Map.entry(android.content.Context.USB_SERVICE,android.hardware.usb.IUsbManager.class.getName()),
        Map.entry("permission",android.os.IPermissionController.class.getName()),
        Map.entry(android.content.Context.TRUST_SERVICE,android.app.trust.ITrustManager.class.getName()),
        Map.entry(android.content.Context.APPWIDGET_SERVICE,com.android.internal.appwidget.IAppWidgetService.class.getName()),
        Map.entry("package_native",android.content.pm.IPackageManagerNative.class.getName()),
        Map.entry("telephony.registry",com.android.internal.telephony.ITelephonyRegistry.class.getName()),
        Map.entry(android.content.Context.SENSOR_PRIVACY_SERVICE,android.hardware.ISensorPrivacyManager.class.getName()),
        Map.entry("scheduling_policy",android.os.ISchedulingPolicyService.class.getName()),
        Map.entry("processinfo",android.os.IProcessInfoService.class.getName()));}
    private final android.content.Context context;
    private final maclauncher.android.LocalProviders providers;
    private final DropBoxManagerService diagnosticStore;
    private final HostStats statistics;
    private final HostUsb usb;
    private HostPermissionController permissions;
    private HostSensorPrivacy sensorPrivacy;
    private HostTelephonyRegistry telephony;
    private HostAudioPermissions audioPermissions;
    private HostNativePackages nativePackages;
    private HostTrustService trust;
    private com.android.server.appwidget.HostWidgetService widgets;
    private com.android.server.HostSchedulingPolicy schedulingPolicy;
    private boolean closed;
    private HostSystemServices(android.content.Context context,maclauncher.android.LocalProviders providers,DropBoxManagerService store,HostStats statistics,HostUsb usb){this.context=context;this.providers=providers;diagnosticStore=store;this.statistics=statistics;this.usb=usb;}
    public static HostSystemServices start(Path runtime,Path data,String processName)throws Exception {
        HostStartupTiming.mark("system-start");
        HostFileObserver.configure(data.toString());
        Path framework=runtime.resolve("android-framework.jar");
        FrameworkResources.configure(framework,List.of(framework));
        HostStartupTiming.mark("system-framework-resources");
        HostContext.hostPackage("android","16",1);
        HostContext.hostApplicationInfo(0,framework.toString(),data.toString(),runtime.resolve("native").toString(),0);
        HostContext.hostApplicationIdentity(android.os.Process.SYSTEM_UID,0);
        // System components share the existing headless display observation;
        // they do not allocate an application window or a second display clock.
        System.load(System.getProperty("klepton.graphics"));
        HostDisplay.initializeHeadless();
        HostStartupTiming.mark("system-headless-graphics");
        HostPackages.inspectSources(List.of(framework),false);
        HostStartupTiming.mark("system-framework-package");
        HostContext.hostProcessName(processName);
        HostStorageService.install();
        // The same original APK loader performs conversion on first component
        // use. Unrequested platform applications have no converter work.
        maclauncher.android.DynamicDex.configure(runtime,data.resolve("cache"),framework,HostSystemServices.class.getClassLoader());
        HostApplication.prepare(HostSystemServices.class.getClassLoader());
        HostStartupTiming.mark("system-application-prepare");
        var applicationContext=HostApplication.applicationContext();
        HostApplication.attach(new Application(),applicationContext);
        var context=applicationContext.createAttributionContext(null);
        new com.android.server.SystemConfigService(context).onStart();
        HostStartupTiming.mark("system-config-service");
        HostPlatformPackages.inspect();
        HostStartupTiming.mark("system-platform-catalog");
        // Content observation uses the same original observer/Handler owner as
        // application providers. No second content tree is created here.
        var providers=new maclauncher.android.LocalProviders();
        providers.install(context,List.of());
        HostStartupTiming.mark("system-providers");
        // The supplied constructor accepts an embedding-owned directory and
        // looper. Upstream retains serialization, quotas, compression, tags,
        // permission checks, content observation and broadcast scheduling.
        var diagnosticStore=new DropBoxManagerService(context,
                data.resolve("system/dropbox").toFile(),Looper.getMainLooper());
        HostStartupTiming.mark("system-dropbox");
        var owner=new HostSystemServices(context,providers,diagnosticStore,new HostStats(context),new HostUsb(context));
        HostStartupTiming.mark("system-stats-usb");
        try {
            owner.permissions=new HostPermissionController(context);
            owner.trust=new HostTrustService(context);
            HostStartupTiming.mark("system-permissions-trust");
            HostServices.registerLazy(android.content.Context.APPWIDGET_SERVICE,
                com.android.internal.appwidget.IAppWidgetService.class.getName(),owner::widgets,true,owner::onMain);
            owner.nativePackages=new HostNativePackages(context);
            owner.telephony=new HostTelephonyRegistry(context);
            HostServices.registerLazy(android.content.Context.SENSOR_PRIVACY_SERVICE,
                android.hardware.ISensorPrivacyManager.class.getName(),owner::sensorPrivacy,true,owner::onMain);
            HostStartupTiming.mark("system-package-telephony-sensors");
            owner.audioPermissions=new HostAudioPermissions(context);
            HostStartupTiming.mark("system-audio-permissions");
            owner.schedulingPolicy=new com.android.server.HostSchedulingPolicy();
            diagnosticStore.onStart();
            diagnosticStore.onBootPhase(SystemService.PHASE_SYSTEM_SERVICES_READY);
            diagnosticStore.onBootPhase(SystemService.PHASE_BOOT_COMPLETED);
            owner.statistics.ready();
            owner.usb.ready();
            HostStartupTiming.mark("system-ready");
            return owner;
        } catch(Throwable failure){try{owner.close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }
    private void onMain(Runnable operation){
        if(Looper.myLooper()==context.getMainLooper())operation.run();
        else if(!new Handler(context.getMainLooper()).post(operation))throw new IllegalStateException("Android system service owner has stopped");
    }
    private synchronized com.android.server.appwidget.HostWidgetService widgets(){
        if(closed)throw new IllegalStateException("Android system services have stopped");
        if(widgets==null)widgets=new com.android.server.appwidget.HostWidgetService(context.createAttributionContext(null));
        return widgets;
    }
    private synchronized android.hardware.ISensorPrivacyManager sensorPrivacy(){
        if(closed)throw new IllegalStateException("Android system services have stopped");
        if(sensorPrivacy==null){
            var service=new HostSensorPrivacy(context);
            try{service.ready();sensorPrivacy=service;}
            catch(RuntimeException|Error failure){try{service.close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}
        }
        return sensorPrivacy.endpoint();
    }
    @Override public synchronized void close(){
        if(closed)return;closed=true;
        HostServices.unregister(android.content.Context.DROPBOX_SERVICE,diagnosticStore.getServiceStub(),"com.android.internal.os.IDropBoxManagerService");
        statistics.close();
        usb.close();
        if(permissions!=null)permissions.close();
        if(sensorPrivacy!=null)sensorPrivacy.close();
        if(telephony!=null)telephony.close();
        if(audioPermissions!=null)audioPermissions.close();
        if(nativePackages!=null)nativePackages.close();
        if(trust!=null)trust.close();
        HostServices.unregisterLazy(android.content.Context.APPWIDGET_SERVICE);
        HostServices.unregisterLazy(android.content.Context.SENSOR_PRIVACY_SERVICE);
        if(widgets!=null){
            HostServices.unregister(android.content.Context.APPWIDGET_SERVICE,widgets,
                com.android.internal.appwidget.IAppWidgetService.class.getName());
            widgets.close();
        }
        if(schedulingPolicy!=null)schedulingPolicy.close();
        HostApplication.releaseReceivers(context);
        providers.close();
        HostApplication.releaseContext(context,"Android system services");
        HostFileObserver.shutdown();
    }
}
