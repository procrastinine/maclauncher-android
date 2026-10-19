package maclauncher.android.framework;

import android.app.LoadedApk;
import android.content.pm.*;
import android.util.Pair;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.server.pm.PackageSetting;
import com.android.server.pm.parsing.library.PackageBackwardCompatibility;
import dalvik.system.BaseDexClassLoader;
import java.util.*;

/** Installed platform-library records over original configuration, package policy
 * and LoadedApk loaders. No second package installer or class-delegation policy. */
final class HostSharedLibraries {
    private HostSharedLibraries(){}
    private static final class Platform {
        static final Map<String,SharedLibraryInfo> LIBRARIES=libraries();
        private static Map<String,SharedLibraryInfo> libraries(){
            var entries=HostPermissions.platformConfiguration().getSharedLibraries();
            var result=new LinkedHashMap<String,SharedLibraryInfo>();
            for(var entry:entries.values())result.put(entry.name,new SharedLibraryInfo(
                entry.filename,null,null,entry.name,SharedLibraryInfo.VERSION_UNDEFINED,
                SharedLibraryInfo.TYPE_BUILTIN,new VersionedPackage("android",0),null,null,entry.isNative));
            for(var entry:entries.values())if(entry.dependencies!=null)for(String dependency:entry.dependencies)
                result.get(entry.name).addDependency(result.get(dependency));
            return Collections.unmodifiableMap(result);
        }
    }
    static String[] names(){return Platform.LIBRARIES.keySet().toArray(String[]::new);}
    static List<SharedLibraryInfo> libraries(){return List.copyOf(Platform.LIBRARIES.values());}
    static void prepare(ParsedPackage parsed,boolean system){
        PackageBackwardCompatibility.modifySharedLibraries(parsed,system,false);
    }
    static void install(ParsedPackage parsed,PackageSetting state){
        var selected=new LinkedHashSet<SharedLibraryInfo>();
        for(List<String> requested:List.of(parsed.getUsesLibraries(),parsed.getUsesOptionalLibraries(),
                parsed.getUsesNativeLibraries(),parsed.getUsesOptionalNativeLibraries()))
            for(String name:requested){var library=Platform.LIBRARIES.get(name);if(library!=null)selected.add(library);
                else System.err.println("[android-host] Shared library could not be found: "+name);}
        state.getPkgState().setUsesLibraryInfos(new ArrayList<>(selected));
        @SuppressWarnings("unchecked")var dependencies=(List<SharedLibraryInfo>)HostUiAccess.call(
            HostUiAccess.method(HostUiAccess.type("com.android.server.pm.SharedLibraryUtils"),"findSharedLibraries",com.android.server.pm.pkg.PackageStateInternal.class),null,state);
        var paths=new LinkedHashSet<String>();for(var library:dependencies)paths.addAll(library.getAllCodePaths());
        state.setPkgStateLibraryFiles(paths);
    }
    /** The VM supplies the executable application loader. Original LoadedApk
     * supplies its canonical shared-library loaders and before/after ordering. */
    static void bind(LoadedApk apk,ClassLoader loader){
        if(!(loader instanceof BaseDexClassLoader dex))return;
        ApplicationInfo info=apk.getApplicationInfo();
        @SuppressWarnings("unchecked")var libraries=(Pair<List<ClassLoader>,List<ClassLoader>>)HostUiAccess.call(
            HostUiAccess.method(LoadedApk.class,"createSharedLibrariesLoaders",List.class,boolean.class,String.class,String.class),
            apk,info.sharedLibraryInfos,info.isSystemApp()&&!info.isUpdatedSystemApp(),info.nativeLibraryDir,info.dataDir);
        bind(dex,"sharedLibraryLoaders",libraries.first);bind(dex,"sharedLibraryLoadersAfter",libraries.second);
    }
    private static void bind(BaseDexClassLoader loader,String field,List<ClassLoader> supplied){
        if(supplied==null||supplied.isEmpty())return;
        ClassLoader[] previous=(ClassLoader[])HostUiAccess.get(loader,BaseDexClassLoader.class,field);
        var values=new ArrayList<ClassLoader>();if(previous!=null)Collections.addAll(values,previous);
        for(ClassLoader value:supplied)if(!values.contains(value))values.add(value);
        HostUiAccess.set(loader,BaseDexClassLoader.class,field,values.toArray(ClassLoader[]::new));
    }
}
