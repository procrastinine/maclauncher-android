package maclauncher.android;
import android.app.Activity;
import android.view.View;
import java.nio.file.Path;
import maclauncher.android.bridge.*;
import maclauncher.android.framework.HostActivity;
import maclauncher.android.framework.HostApplication;

/** Native macOS host for the installed Android compatibility profile. */
public final class Host {
    public static void main(String[] args){int status=0;try(var workers=FirstFailure.captureWorkers()){try{run(args);}catch(Throwable failure){FirstFailure.reportOwner(failure);status=1;}}System.exit(status);}
    private static void run(String[] args)throws Throwable{
        System.setProperty("java.awt.headless","true");
        if(args.length!=2||!args[0].equals("--config"))throw new IllegalArgumentException("Use --config followed by an Android launch configuration");
        LaunchConfig config=LaunchConfig.read(Path.of(args[1]));
        android.os.Looper mainLooper=null;NativeComposition.Session composition=null;
        ClassLoader previous=Thread.currentThread().getContextClassLoader();
        ClassLoader loader=null;LocalProviders providers=new LocalProviders();Activity activity=null;HostActivity task=null;Throwable failure=null;HostActivity.Window windowSession=null;ProcessAuthority authority=null;AccountControl accountControl=null;AutoCloseable providerOperation=null;PlayLibraryOperation libraryOperation=null;
        try{
            RuntimeBootstrap.initialize(config.directory("runtimeRoot"),config.directory("filesRoot").getParent(),config.directory("filesRoot"),config.directory("cacheRoot"),config.text("packageName"),config.networkAllowed(),config.allowAds(),true);
            authority=ProcessAuthority.read(config);
            if(authority!=null)maclauncher.android.framework.HostProcessIdentity.configure(authority.self.uid(),authority.self.hostUid());
            if(authority!=null)authority.attachPlatform();
            maclauncher.android.framework.HostGoogleProviders.configure(config.googleProviderCompatibility());
            boolean serviceProcess=config.serviceProcess();
            if(!serviceProcess)maclauncher.android.framework.HostRemoteServices.prepareWindowServices();
            boolean accountService=config.accountService();
            boolean packageOperation=config.contains("serviceOperation");
            if(accountService&&(authority==null||authority.self.uid()!=android.os.Process.SYSTEM_UID||!"u:r:system_server:s0".equals(authority.self.securityContext())))throw new SecurityException("Android account service requires its authenticated system identity");
            maclauncher.android.framework.HostStorageService.install();
            ApplicationSettings inputSettings=config.applicationSettings();
            maclauncher.android.framework.HostContextServices.configureInputs(inputSettings);
            Graphics.configureHost(config.optional("graphicsLibrary"),config.runtimeDirectory("native/angle").toString(),(int)config.number("width"),(int)config.number("height"));
            NativeSurface.configureGraphics(System.getProperty("klepton.graphics"));NativeSurface.configureCache(config.directory("cacheRoot").toString());NativeSurface.install(System.getProperty("klepton.bridge"));
            if(!serviceProcess)maclauncher.android.framework.HostActivityManager.hostInitializeGraphics();
            Bridge.configure(config.directory("assetsRoot").toString(),config.directory("filesRoot").toString());
            maclauncher.android.framework.HostFilesystem.installNativeMappings(config.sourceApks(),config.directory("assetsRoot"));
            maclauncher.android.framework.HostContextServices.configureMedia(inputSettings);
            maclauncher.android.framework.HostContext.hostCacheDirectory(config.directory("cacheRoot").toString());
            maclauncher.android.framework.HostFileObserver.configure(config.directory("filesRoot").getParent().toString());
            maclauncher.android.framework.HostContext.hostPackage(config.text("packageName"),config.text("versionName"),(int)config.number("versionCode"));
            mainLooper=maclauncher.android.framework.HostLooper.main();
            maclauncher.android.framework.HostContext.hostApplicationInfo(0,config.file("sourceApk").toString(),config.directory("filesRoot").getParent().toString(),config.directory("nativeRoot").toString(),0);
            maclauncher.android.framework.HostPackages.hostInstallTimes(config.directory("filesRoot").getParent(),config.file("sourceApk"));
            var sourceApks=config.sourceApks();
            maclauncher.android.framework.FrameworkResources.configure(config.runtimeFile("android-framework.jar"),sourceApks);
            maclauncher.android.framework.HostContext.hostApplicationIdentity(android.os.Process.myUid(),0);
            var inspected=maclauncher.android.framework.HostPackages.inspectSources(sourceApks);
            maclauncher.android.framework.HostPackages.inspectInstalled(config.optional("installedPackagesJson"));
            if(accountService&&config.contains("accountProviderPackage")){String provider=config.text("accountProviderPackage");maclauncher.android.framework.HostAccountIdentity.bind(config.directory("filesRoot").getParent(),provider,maclauncher.android.framework.HostPackages.verifiedSigningDetails(provider));}
            if(config.contains("accountControlFd")&&!accountService)maclauncher.android.framework.HostAccountIdentity.bind(config.directory("filesRoot").getParent(),config.text("packageName"),maclauncher.android.framework.HostPackages.verifiedCurrentSigningDetails());
            var applicationInfo=maclauncher.android.framework.HostContext.hostApplicationInfoSnapshot();
            maclauncher.android.framework.HostFilesystem.installNativeMappings(sourceApks,config.directory("assetsRoot"),applicationInfo);
            var activityInfo=serviceProcess?null:maclauncher.android.framework.HostPackages.activityInfo(config.text("activityClass"));
            String processName=serviceProcess?config.text("processName"):activityInfo.processName==null?applicationInfo.processName:activityInfo.processName;
            if(!accountService&&!maclauncher.android.framework.HostPackages.declaredProcess(processName))throw new IllegalArgumentException("Android process is not declared by the installed APK");
            if(authority!=null&&!authority.self.processName().equals(processName))throw new SecurityException("Android process declaration differs from its supervisor identity");
            maclauncher.android.framework.HostContext.hostProcessName(processName);
            maclauncher.android.framework.HostActivityManager.hostExitHistory(config.optional("processExitHistoryJson"));
            loader=accountService?Host.class.getClassLoader():maclauncher.android.framework.HostDexFiles.application(config.file("convertedJar"),sourceApks,config.directory("nativeRoot"),Host.class.getClassLoader());
            Thread.currentThread().setContextClassLoader(loader);maclauncher.android.framework.HostContext.hostClassLoader(loader);
            if(!accountService){GuestNativeLoader.configure(System.getProperty("klepton.bridge"),config.directory("nativeRoot").toString(),config.file("convertedJar").toString(),loader);
            DynamicDex.configure(config.directory("runtimeRoot"),config.directory("cacheRoot"),config.file("convertedJar"),loader);}
            composition=authority==null?null:authority.composition();
            if(composition!=null){if(serviceProcess)throw new SecurityException("Native activity output was supplied to a service process");NativeCompositionWindow.install(composition,config.descriptor("nativeInputFd"),config.descriptor("nativeControlFd"));}
            else{if(config.contains("nativeInputFd")||config.contains("nativeControlFd"))throw new SecurityException("Native activity output has no authenticated composition owner");WindowSession.configureIdentity(config.text("title"),config.icon(),serviceProcess);}
            if(composition!=null)WindowSession.configureIdentity(config.text("title"),config.icon(),true);
            if(!serviceProcess){var viewport=HostActivity.initialWindowSize(activityInfo,(int)config.number("width"),(int)config.number("height"));windowSession=HostActivity.openWindow(config.text("title"),viewport.x,viewport.y,authority==null?null:authority::windowCloseRequested);}
            else maclauncher.android.framework.HostDisplay.initializeHeadless();
            maclauncher.android.framework.HostContextServices.configureBrowser(config.directory("filesRoot"),config.networkAllowed(),config.webLinksAllowed(),config.runtimeFile("android-host.jar"));
            HostApplication.prepare(loader);
            android.content.Context applicationContext=HostApplication.applicationContext();
            applicationContext.setTheme(applicationInfo.theme);
            android.app.Application application;
            if(accountService||packageOperation){application=new android.app.Application();HostApplication.attach(application,applicationContext);}
            else application=HostApplication.instantiate(loader,applicationInfo.className);
            maclauncher.android.framework.HostContextServices.applicationCreated(application,mainLooper);
            if(serviceProcess&&authority==null)throw new SecurityException("Android service process requires its supervisor authority");
            if(authority!=null){var loop=mainLooper;authority.monitor(applicationContext,()->new android.os.Handler(loop).post(()->maclauncher.android.framework.HostLooper.close(loop)));}
            var processProviders=inspected.stream().filter(info->java.util.Objects.equals(info.processName,processName)).toList();
            if(!accountService&&!packageOperation){
                providerOperation=maclauncher.android.framework.HostContextServices.installProviders(application,processProviders,providers,config.contains("googleFreeAcquisition"));
            }
            HostApplication.create(application);
            if(config.contains("accountControlFd")){
                var loop=mainLooper;
                try(var socket=android.os.ParcelFileDescriptor.adoptFd(config.descriptor("accountControlFd"))){
                    accountControl=new AccountControl(applicationContext,socket,config.optional("accountSocketPath"),intent->HostActivity.request(applicationContext,intent),()->maclauncher.android.framework.HostLooper.close(loop));
                }
            }
            if(packageOperation)libraryOperation=new PlayLibraryOperation(applicationContext,authority,config.text("operationId"));
            if(serviceProcess){android.os.Looper.loop();return;}
            activity=HostActivity.instantiate(loader,config.text("activityClass"));
            HostActivity.attach(activity,applicationContext,application,activityInfo);
            task=HostActivity.owner(activity);
            activity.setTheme(activityInfo.getThemeResource());
            windowSession.attachActivity(activity);
            HostActivity.start(activity,new android.os.Bundle());
            // A launcher Activity may queue its successor and finish in
            // onCreate. The presenter drains the task owner before deciding
            // whether the task is empty, just as for later navigation.
            View view=HostActivity.content(activity);
            if(view==null)throw new IllegalStateException("Android activity did not create a window view");
            int viewportWidth=maclauncher.android.framework.HostDisplay.host().getWidth(),viewportHeight=maclauncher.android.framework.HostDisplay.host().getHeight();
            // Activities can introduce or retire a GLES view after launch. The
            // window's one presenter supports Canvas and native producers alike.
            windowSession.present(activity,config.text("title"),viewportWidth,viewportHeight);
        }catch(Throwable error){failure=error;FirstFailure.reportOwner(error);throw error;}
        finally{
            Throwable cleanup=failure;
            try{if(libraryOperation!=null)libraryOperation.close();}catch(Throwable error){cleanup=keep(cleanup,error);}
            try{if(providerOperation!=null)providerOperation.close();}catch(Throwable error){cleanup=keep(cleanup,error);}
            try{if(accountControl!=null)accountControl.close();}catch(Throwable error){cleanup=keep(cleanup,error);}
            try{maclauncher.android.framework.HostRemoteServices.shutdown();}catch(Throwable error){cleanup=keep(cleanup,error);}
            try{if(authority!=null)authority.close();}catch(Throwable error){cleanup=keep(cleanup,error);}
            // This dedicated JVM exits immediately after lifecycle cleanup.
            // SDK workers and shutdown hooks may still need guest classes, so
            // let process exit release these loaders rather than closing their
            // archives beneath live threads. Embedded callers of finish retain
            // explicit loader cleanup when they supply one.
            Throwable result=finish(cleanup,task,providers,null,mainLooper,previous,windowSession);
            if(windowSession==null&&composition!=null)try{composition.close();}catch(Throwable error){result=keep(result,error);}
            if(failure==null&&result!=null)throw result;
        }
    }
    private static Throwable keep(Throwable first,Throwable next){if(first==null)return next;if(first!=next)first.addSuppressed(next);return first;}
    /** Release the Android compositor while its native output and callbacks remain live. */
    public static void finishPresentation()throws Throwable{
        maclauncher.android.framework.HostWindowManager.detachPresentation();
    }
    /** Shared finalization also covers failures before an Activity or loader exists. */
    static Throwable finish(Throwable first,HostActivity task,LocalProviders providers,AutoCloseable loader,android.os.Looper looper,ClassLoader previous){
        return finish(first,task,providers,loader,looper,previous,null);
    }
    static Throwable finish(Throwable first,HostActivity task,LocalProviders providers,AutoCloseable loader,android.os.Looper looper,ClassLoader previous,HostActivity.Window session){
        first=retire(first,task,providers,loader,looper,session);
        try{Thread.currentThread().setContextClassLoader(previous);}catch(Throwable error){first=keep(first,error);}
        return first;
    }
    private static Throwable retire(Throwable first,HostActivity task,LocalProviders providers,AutoCloseable loader,android.os.Looper looper,HostActivity.Window session){
        // Activity callbacks may wait for their render thread and display clock.
        // The renderer normally retires the task. The same owner also covers
        // failures before adoption, navigation and replacement Activity objects.
        try{if(task!=null)task.finish();}catch(Throwable error){first=keep(first,error);}
        // The renderer normally performs this before releasing its presenter.
        // Cover failures before adoption, too: ViewRoot and compositor owners
        // must release their buffers before the native session can be closed.
        try{finishPresentation();}catch(Throwable error){return keep(first,error);}
        // Input devices, the WindowManager service/session and SurfaceTexture
        // consumers belong to the process. Retire them only at process exit,
        // while the current display callback barriers can still drain work.
        try{maclauncher.android.framework.HostWindowManager.shutdown();}catch(Throwable error){return keep(first,error);}
        try{maclauncher.android.framework.HostSurfaceTexture.shutdown();}catch(Throwable error){return keep(first,error);}
        // No dependency may be released beneath an executing display callback.
        // Failed retirement leaves the complete process-owned graph alive for
        // process exit or an explicit retry, rather than just its native window.
        try{if(session!=null)session.stopCallbacks(()->maclauncher.android.framework.HostLooper.drain(looper));}
        catch(Throwable error){return keep(first,error);}
        // A claimed session cannot close while a producer still owns it. This
        // second retirement barrier protects shared resources after render errors.
        try{if(session!=null)session.close();}catch(Throwable error){return keep(first,error);}
        try{NativeSurface.shutdownDisplayCallbacks();}catch(Throwable error){first=keep(first,error);}
        try{maclauncher.android.framework.HostDisplayEvents.hostShutdown();}catch(Throwable error){first=keep(first,error);}
        try{maclauncher.android.framework.HostDisplay.hostShutdown();maclauncher.android.framework.HostDisplay.hostRemove();}catch(Throwable error){first=keep(first,error);}
        try{maclauncher.android.framework.HostFileObserver.shutdown();}catch(Throwable error){first=keep(first,error);}
        first=maclauncher.android.framework.HostContextServices.retireApplication(first,providers);
        try{DynamicDex.hostShutdown();}catch(Throwable error){first=keep(first,error);}
        try{maclauncher.android.framework.HostLooper.close(looper);}catch(Throwable error){first=keep(first,error);}
        first=maclauncher.android.framework.HostContextServices.retireContext(first,looper,loader);
        try{WindowSession.shutdownApplication();}catch(Throwable error){first=keep(first,error);}
        return first;
    }
}
