package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.content.pm.ApplicationInfo;
import android.content.res.*;
import java.util.*;

/** Binds Android's real process records to the inspected application and VM loader. */
public final class HostApplication {
    private static HostApplication current;
    private static boolean modulesInitialized;
    final Thread owner=Thread.currentThread();
    final ActivityThread thread;
    final LoadedApk apk;
    final Instrumentation instrumentation;
    final Context context;
    private Application application;
    private Configuration pendingConfiguration;
    private final Runnable configurationDispatch=this::dispatchConfiguration;
    private final HostActivityService services;
    private HostApplication(ClassLoader loader){
        HostStartupTiming.mark("application-records-start");
        if(ActivityThread.currentActivityThread()!=null)throw new IllegalStateException("An Android application process is already bound");
        thread=(ActivityThread)HostUiAccess.construct(ActivityThread.class,new Class<?>[0]);
        HostUiAccess.set(thread,ActivityThread.class,"mProfiler",HostUiAccess.construct(HostUiAccess.type("android.app.ActivityThread$Profiler"),new Class<?>[0]));
        ApplicationInfo info=HostContext.hostApplicationInfoSnapshot();
        Configuration resourceConfiguration=new Configuration(ResourcesManager.getInstance().getConfiguration());
        // ActivityThread binds the process records before application code can
        // initialize its component factory. A non-null base loader would run
        // that factory inside LoadedApk's constructor, before those records exist.
        apk=new LoadedApk(thread,info,CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,null,false,true,false);
        // These are the actual LoadedApk platform bindings, supplied by the launcher
        // instead of the Android zygote/package server. Generic LoadedApk APIs remain intact.
        HostUiAccess.set(apk,LoadedApk.class,"mClassLoader",Objects.requireNonNull(loader));
        HostUiAccess.set(apk,LoadedApk.class,"mDefaultClassLoader",loader);
        instrumentation=new Instrumentation();
        HostUiAccess.call(HostUiAccess.method(Instrumentation.class,"basicInit",ActivityThread.class),instrumentation,thread);
        Class<?> bindType=HostUiAccess.type("android.app.ActivityThread$AppBindData");
        Object bind=HostUiAccess.construct(bindType,new Class<?>[0]);
        HostUiAccess.set(bind,bindType,"info",apk);
        HostUiAccess.set(bind,bindType,"appInfo",info);
        HostUiAccess.set(bind,bindType,"processName",HostContext.hostProcessName());
        HostUiAccess.set(bind,bindType,"config",new Configuration(resourceConfiguration));
        HostUiAccess.set(bind,bindType,"compatInfo",CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO);
        HostUiAccess.set(thread,ActivityThread.class,"mBoundApplication",bind);
        HostUiAccess.set(thread,ActivityThread.class,"mInstrumentation",instrumentation);
        HostStartupTiming.mark("application-fonts-start");
        HostSystemFonts.initialize((android.os.SharedMemory)HostUiAccess.get(bind,bindType,"mSerializedSystemFontMap"));
        HostStartupTiming.mark("application-fonts-ready");
        Class<?> controller=HostUiAccess.type("android.app.ConfigurationController");
        Object configuration=HostUiAccess.construct(controller,new Class<?>[]{HostUiAccess.type("android.app.ActivityThreadInternal")},thread);
        HostUiAccess.call(HostUiAccess.method(controller,"setConfiguration",Configuration.class),configuration,new Configuration(resourceConfiguration));
        HostUiAccess.call(HostUiAccess.method(controller,"setCompatConfiguration",Configuration.class),configuration,new Configuration(resourceConfiguration));
        HostUiAccess.set(thread,ActivityThread.class,"mConfigurationController",configuration);
        // ActivityThread's original bind path establishes process bitmap density
        // together with resource configuration. The host supplies the process
        // record; the upstream controller retains density/compatibility policy.
        if((info.flags&ApplicationInfo.FLAG_SUPPORTS_SCREEN_DENSITIES)==0){
            HostUiAccess.set(thread,ActivityThread.class,"mDensityCompatMode",true);
            android.graphics.Bitmap.setDefaultDensity(android.util.DisplayMetrics.DENSITY_DEFAULT);
        }
        HostUiAccess.call(HostUiAccess.method(controller,"updateDefaultDensity",int.class),configuration,resourceConfiguration.densityDpi);
        HostUiAccess.set(null,ActivityThread.class,"sCurrentActivityThread",thread);
        // ActivityThread.main normally publishes the handler after process attach.
        // The host owns that entry point but all framework dispatch uses its real H.
        HostUiAccess.set(null,ActivityThread.class,"sMainThreadHandler",thread.getHandler());
        @SuppressWarnings("unchecked")var packages=(Map<String,java.lang.ref.WeakReference<LoadedApk>>)HostUiAccess.get(thread,ActivityThread.class,"mPackages");packages.put(info.packageName,new java.lang.ref.WeakReference<>(apk));
        @SuppressWarnings("unchecked")var resourcePackages=(Map<String,java.lang.ref.WeakReference<LoadedApk>>)HostUiAccess.get(thread,ActivityThread.class,"mResourcePackages");resourcePackages.put(info.packageName,new java.lang.ref.WeakReference<>(apk));
        context=(Context)HostUiAccess.call(HostUiAccess.method(HostUiAccess.type("android.app.ContextImpl"),"createAppContext",ActivityThread.class,LoadedApk.class),null,thread,apk);
        services=new HostActivityService(context);
        HostStartupTiming.mark("application-records-ready");
    }
    private void initializeClassLoader(ClassLoader loader){
        HostSharedLibraries.bind(apk,loader);
        AppComponentFactory factory=(AppComponentFactory)HostUiAccess.call(HostUiAccess.method(LoadedApk.class,"createAppFactory",ApplicationInfo.class,ClassLoader.class),apk,apk.getApplicationInfo(),loader);
        HostUiAccess.set(apk,LoadedApk.class,"mAppComponentFactory",factory);
        HostUiAccess.set(apk,LoadedApk.class,"mClassLoader",factory.instantiateClassLoader(loader,new ApplicationInfo(apk.getApplicationInfo())));
    }
    public static synchronized void prepare(ClassLoader loader){
        prepare(loader,new HostContextServices.PhysicalBindings(false,new maclauncher.android.media.HostAudioManager()));
    }
    public static synchronized HostWindowService prepare(ClassLoader loader,HostContextServices.PhysicalBindings physical){
        HostStartupTiming.mark("application-prepare-start");
        if(current!=null)throw new IllegalStateException("Application is already prepared");
        HostApplicationSharedMemory.initialize();
        if(!modulesInitialized){ActivityThread.initializeMainlineModules();modulesInitialized=true;}
        // Zygote normally installs the original provider before app code runs.
        // Both VM entries bind applications directly, without that preload path.
        android.security.keystore2.AndroidKeyStoreProvider.install();
        HostStartupTiming.mark("application-mainline-keystore");
        HostDisplay.install();current=new HostApplication(loader);var windows=HostContextServices.install(current.context,physical);HostActivity.install();
        HostStartupTiming.mark("application-context-services");
        android.security.net.config.NetworkSecurityConfigProvider.install(current.context);
        current.initializeClassLoader(loader);
        HostStartupTiming.mark("application-prepare-ready");
        return windows;
    }
    public static synchronized Context applicationContext(){if(current==null)prepare(HostContext.hostClassLoader());return current.context;}
    static synchronized void displayConfigurationChanged(Configuration config){
        if(current==null)return;current.check();
        Object configuration=HostUiAccess.get(current.thread,ActivityThread.class,"mConfigurationController");
        HostUiAccess.call(HostUiAccess.method(configuration.getClass(),"updateDefaultDensity",int.class),configuration,config.densityDpi);
        if(current.application==null)return;
        boolean queued=current.pendingConfiguration!=null;
        current.pendingConfiguration=new Configuration(config);
        if(!queued)current.thread.getHandler().post(current.configurationDispatch);
    }
    private void dispatchConfiguration(){
        check();Configuration config=pendingConfiguration;pendingConfiguration=null;
        if(config==null||application==null)return;
        // Native geometry is committed before this notification. The original
        // controller owns Application/Service/Provider callbacks and resources;
        // original Activity transactions retain their own lifecycle policy.
        thread.handleConfigurationChanged(config,Context.DEVICE_ID_DEFAULT);
        HostActivity.configurationChanged(config);
        var metrics=new android.util.DisplayMetrics();HostDisplay.metrics(metrics);
        HostWindowManager.resizeOutput(metrics.widthPixels,metrics.heightPixels);
    }
    static Context implementation(Context context){return (Context)HostUiAccess.call(HostUiAccess.method(HostUiAccess.type("android.app.ContextImpl"),"getImpl",Context.class),null,Objects.requireNonNull(context));}
    static void outerContext(Context context,Context outer){HostUiAccess.call(HostUiAccess.method(HostUiAccess.type("android.app.ContextImpl"),"setOuterContext",Context.class),implementation(context),Objects.requireNonNull(outer));}
    static Context activityContext(android.content.pm.ActivityInfo info,android.os.IBinder token){HostApplication process=current();return (Context)HostUiAccess.call(HostUiAccess.method(HostUiAccess.type("android.app.ContextImpl"),"createActivityContext",ActivityThread.class,LoadedApk.class,android.content.pm.ActivityInfo.class,android.os.IBinder.class,int.class,Configuration.class),null,process.thread,process.apk,info,token,android.view.Display.DEFAULT_DISPLAY,new Configuration(process.context.getResources().getConfiguration()));}
    static HostApplication current(){HostApplication value=current;if(value==null)throw new IllegalStateException("No application process is bound");value.check();return value;}
    private void check(){if(Thread.currentThread()!=owner)throw new IllegalStateException("Application lifecycle requires its owner thread");}
    /** Original Instrumentation preserves the APK's AppComponentFactory and attaches exactly once. */
    public static Application instantiate(ClassLoader loader,String name)throws ReflectiveOperationException{
        HostApplication value=current();if(value.application!=null)throw new IllegalStateException("Application has already been attached");
        Objects.requireNonNull(loader);
        Application application=value.instrumentation.newApplication(value.apk.getClassLoader(),name==null||name.isBlank()?Application.class.getName():name,value.context);
        adopt(application,value.context);return application;
    }
    public static void attach(Application application,Context context){
        HostApplication value=current();if(value.application!=null)throw new IllegalStateException("Application has already been attached");
        HostUiAccess.call(HostUiAccess.method(Application.class,"attach",Context.class),Objects.requireNonNull(application),context);
        adopt(application,context);
    }
    private static void adopt(Application application,Context context){
        HostApplication value=current();if(value.application!=null)throw new IllegalStateException("Application has already been attached");
        outerContext(context,application);
        value.application=application;
        HostUiAccess.set(value.thread,ActivityThread.class,"mInitialApplication",application);
        HostUiAccess.set(value.apk,LoadedApk.class,"mApplication",application);
        @SuppressWarnings("unchecked")var applications=(List<Application>)HostUiAccess.get(value.thread,ActivityThread.class,"mAllApplications");applications.add(application);
    }
    public static void create(Application application){HostApplication value=current();if(value.application!=application)throw new IllegalStateException("Application is not attached");value.instrumentation.callApplicationOnCreate(application);}
    /** Original LoadedApk factory retains the declared Service and application loader. */
    static Service instantiateService(String name,Intent intent)throws ReflectiveOperationException{HostApplication value=current();return value.apk.getAppFactory().instantiateService(value.apk.getClassLoader(),name,intent);}
    public static void attachService(Service service,Application application){attachService(service,application,new android.os.Binder());}
    static void attachService(Service service,Application application,android.os.IBinder token){attachService(service,application,token,service.getClass().getName());}
    static void attachService(Service service,Application application,android.os.IBinder token,String name){HostApplication value=current();if(value.application!=application)throw new IllegalArgumentException("Service belongs to another application");Context context=(Context)HostUiAccess.call(HostUiAccess.method(HostUiAccess.type("android.app.ContextImpl"),"createAppContext",ActivityThread.class,LoadedApk.class),null,value.thread,value.apk);outerContext(context,service);service.attach(context,value.thread,name,token,application,ActivityManager.getService());}
    public static void releaseContext(Context context,String owner){HostUiAccess.call(HostUiAccess.method(HostUiAccess.type("android.app.ContextImpl"),"performFinalCleanup",String.class,String.class),implementation(context),owner,"Context");}
    /** Retires registrations made by a host-owned service Context through the
     * original LoadedApk receiver owner, before releasing that Context. */
    @SuppressWarnings("unchecked") public static void releaseReceivers(Context context){
        Object impl=implementation(context);
        Object apk=HostUiAccess.get(impl,impl.getClass(),"mPackageInfo");
        var receivers=(Map<Context,Map<BroadcastReceiver,?>>)HostUiAccess.get(apk,LoadedApk.class,"mReceivers");
        List<BroadcastReceiver> snapshot;
        synchronized(receivers){var owned=receivers.get(context);snapshot=owned==null?List.of():new ArrayList<>(owned.keySet());}
        for(var receiver:snapshot)context.unregisterReceiver(receiver);
    }
    public static synchronized void shutdown(){
        if(current==null)return;
        current.check();Throwable first=null;
        current.thread.getHandler().removeCallbacks(current.configurationDispatch);current.pendingConfiguration=null;
        try{HostAccounts.shutdown();}catch(Throwable failure){first=failure;}
        try{HostLocalServices.shutdown();}catch(Throwable failure){if(first==null)first=failure;else if(first!=failure)first.addSuppressed(failure);}
        try{releaseContext(current.context,"Application");}catch(Throwable failure){if(first==null)first=failure;else if(first!=failure)first.addSuppressed(failure);}
        try{current.services.close();}catch(Throwable failure){if(first==null)first=failure;else if(first!=failure)first.addSuppressed(failure);}
        HostUiAccess.set(null,ActivityThread.class,"sMainThreadHandler",null);
        HostUiAccess.set(null,ActivityThread.class,"sCurrentActivityThread",null);
        current=null;if(first!=null)HostUiAccess.raise(first);
    }
}
