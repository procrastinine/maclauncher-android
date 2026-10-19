package maclauncher.android.framework;

import android.app.ResourcesManager;
import android.content.pm.ApplicationInfo;
import android.content.res.*;
import android.util.DisplayMetrics;
import java.io.IOException;
import java.nio.file.Path;
import java.util.*;

/** APK paths and native display configuration supplied to Android's resource manager. */
public final class FrameworkResources {
    private FrameworkResources(){}
    private record Sources(Path framework,List<String> apks,List<String> splits){}
    private static Sources installed;

    public static synchronized void configure(Path framework,List<Path> sources)throws IOException{
        if(installed!=null)throw new IllegalStateException("Android resources are already configured");
        if(sources.isEmpty())throw new IllegalArgumentException("Invalid Android source APK list");
        try{Class.forName("libcore.util.NativeAllocationRegistry");}catch(ClassNotFoundException error){throw new IllegalStateException(error);}
        Path frameworkSource=framework.toRealPath();AssetManager.createSystemAssetsInZygoteLocked(false,frameworkSource.toString());
        ArrayList<String> paths=new ArrayList<>(sources.size());for(Path source:sources)paths.add(source.toRealPath().toString());
        HostDisplay.install();ResourcesManager manager=ResourcesManager.getInstance();
        Configuration config=new Configuration();config.setToDefaults();config.setLocale(Locale.getDefault());config.keyboard=Configuration.KEYBOARD_NOKEYS;config.hardKeyboardHidden=Configuration.HARDKEYBOARDHIDDEN_YES;config.touchscreen=Configuration.TOUCHSCREEN_FINGER;config.uiMode=Configuration.UI_MODE_TYPE_NORMAL;
        viewport(config);manager.applyConfigurationToResources(config,CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO);
        installed=new Sources(frameworkSource,List.copyOf(paths),null);
    }
    /** LoadedApk and its original ContextImpl must see the same inspected sources as assets. */
    static synchronized void bindPackageSources(ApplicationInfo info){
        if(installed==null)return;List<String> packagePaths=installed.apks();info.sourceDir=info.publicSourceDir=packagePaths.getFirst();
        if(installed.splits()!=null)info.splitNames=installed.splits().toArray(String[]::new);
        info.splitSourceDirs=packagePaths.size()==1?null:packagePaths.subList(1,packagePaths.size()).toArray(String[]::new);
        info.splitPublicSourceDirs=info.splitSourceDirs==null?null:info.splitSourceDirs.clone();
    }
    static synchronized Path frameworkPath(){if(installed==null)throw new IllegalStateException("Android framework resources are not configured");return installed.framework();}
    static synchronized void adoptPackageSources(ApplicationInfo info){
        ArrayList<String> paths=new ArrayList<>();paths.add(info.sourceDir);if(info.splitSourceDirs!=null)Collections.addAll(paths,info.splitSourceDirs);
        if(installed==null)throw new IllegalStateException("Android framework resources are not configured");List<String> packagePaths=installed.apks();
        if(paths.size()!=packagePaths.size()||!sourceIdentities(paths).equals(sourceIdentities(packagePaths)))throw new IllegalArgumentException("Parsed APK sources differ from the installed source set");
        installed=new Sources(installed.framework(),List.copyOf(paths),info.splitNames==null?null:List.of(info.splitNames));
    }
    private static Set<Path> sourceIdentities(List<String> paths){
        var identities=new HashSet<Path>();
        try{for(String source:paths)identities.add(Path.of(source).toRealPath());}
        catch(IOException failure){throw new IllegalArgumentException("Android source APK path could not be resolved",failure);}
        return identities;
    }
    private static void viewport(Configuration config){
        var metrics=new DisplayMetrics();HostDisplay.metrics(metrics);
        int width=Math.max(1,Math.round(metrics.widthPixels/metrics.density)),height=Math.max(1,Math.round(metrics.heightPixels/metrics.density));
        config.screenWidthDp=width;config.screenHeightDp=height;config.smallestScreenWidthDp=Math.min(width,height);config.densityDpi=metrics.densityDpi;
        var bounds=new android.graphics.Rect(0,0,metrics.widthPixels,metrics.heightPixels);
        config.windowConfiguration.setBounds(bounds);config.windowConfiguration.setAppBounds(bounds);config.windowConfiguration.setMaxBounds(bounds);
        config.windowConfiguration.setWindowingMode(android.app.WindowConfiguration.WINDOWING_MODE_FULLSCREEN);
        config.orientation=width>=height?Configuration.ORIENTATION_LANDSCAPE:Configuration.ORIENTATION_PORTRAIT;
        config.screenLayout=Configuration.reduceScreenLayout(Configuration.resetScreenLayout(config.screenLayout),Math.max(width,height),Math.min(width,height));
    }
    /** Published after native surface storage and its real display observation commit. */
    public static synchronized void viewportChanged(){
        if(installed==null)return;
        var manager=ResourcesManager.getInstance();Configuration previous=manager.getConfiguration();Configuration config=new Configuration(previous);viewport(config);
        if(previous.diff(config)==0)return;
        // Activity transactions compare pending and applied original configs.
        // Zero means an unspecified/newer sequence, even against itself.
        config.seq=previous.seq+1;if(config.seq==0)config.seq=1;
        manager.applyConfigurationToResources(config,CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO);
        HostApplication.displayConfigurationChanged(config);
    }
    public static synchronized void shutdown(){
        // This is package metadata, not ownership of ResourcesManager's shared
        // objects. Existing contexts keep their resources until upstream retires them.
        installed=null;
    }
}
