package maclauncher.android.framework;

import android.app.ActivityThread;
import android.content.Context;
import android.content.pm.ProviderInfo;
import android.os.*;
import java.io.IOException;
import java.nio.file.*;
import java.util.*;
import org.json.*;

/** Original supplied APK components share their manifest-declared system owner. */
final class HostPlatformPackages {
    private static final Set<String> pending=new HashSet<>();
    private record PackageSource(Path image,JSONObject entry){}
    private static List<PackageSource> sources=List.of();
    private HostPlatformPackages(){}
    static void inspect()throws Exception {
        Path runtime=HostFilesystem.runtimeRoot();
        sources=List.of();
        var discovered=new ArrayList<PackageSource>();
        // ART sees its original package catalog through the read-only system
        // mapping. JVM packages can also supply the original platform APKs
        // independently of an ART image or any optional Google integration.
        if(runtime.equals(Path.of("/"))){
            inspectCatalog(runtime,Path.of("/system/etc/maclauncher-platform-packages.json"),discovered);
        }else{
            inspectCatalog(runtime.resolve("art/image"),runtime.resolve("art/config/platform-packages.json"),discovered);
            inspectCatalog(runtime.resolve("platform"),runtime.resolve("platform/packages.json"),discovered);
        }
        sources=List.copyOf(discovered);
    }
    private static void inspectCatalog(Path image,Path catalog,List<PackageSource> discovered)throws Exception {
        if(!Files.isRegularFile(catalog))return;
        JSONObject value=new JSONObject(new String(Files.readAllBytes(catalog),java.nio.charset.StandardCharsets.UTF_8));
        if(value.getInt("schemaVersion")!=1)throw new IOException("Android platform package catalog version is unsupported");
        JSONArray packages=value.getJSONArray("packages");
        for(int i=0;i<packages.length();i++){
            JSONObject entry=packages.getJSONObject(i);
            // Shared-user ownership comes from the supplied manifest, never a
            // package-name, authority, method or class admission list.
            if(!"android.uid.system".equals(entry.optString("sharedUserId")))continue;
            discovered.add(new PackageSource(image,entry));
        }
    }
    /** The manifest index only locates source bytes. The original parser still
     * supplies every component, permission, alias and enabled-state decision. */
    static void resolve(String authority)throws Exception {
        for(var candidate:sources){
            JSONObject entry=candidate.entry;JSONArray authorities=entry.getJSONArray("authorities");boolean matches=!entry.getBoolean("resolvedAuthorities");
            for(int i=0;i<authorities.length();i++)if(authority.equals(authorities.getString(i)))matches=true;
            if(!matches||HostPackages.platformPackagePresent(entry.getString("packageName")))continue;
            Path image=candidate.image,relative=Path.of(entry.getString("source")).normalize();
            if(relative.isAbsolute()||relative.startsWith(".."))throw new IOException("Invalid Android platform APK path");
            Path source=image.resolve(relative).toRealPath();
            if(!source.startsWith(image.toRealPath()))throw new IOException("Android platform APK is outside its supplied image");
            HostPackages.inspectPlatform(source,entry.getString("packageName"));
        }
    }
    static boolean owns(ProviderInfo info){return HostPackages.platformComponent(info)&&info.applicationInfo.uid==android.os.Process.SYSTEM_UID&&"android.system".equals(info.processName);}
    static void demand(Context context,ProviderInfo info){
        if(!owns(info))return;
        synchronized(pending){if(!pending.add(info.packageName))return;}
        Runnable install=()->{
            try{
                var thread=Objects.requireNonNull(ActivityThread.currentActivityThread());
                thread.installSystemProviders(HostPackages.platformProviders(info.packageName));
                // SystemServer normally records this value during boot. This
                // profile's authenticated boot owner supplies the same input.
                if(Arrays.asList(info.authority.split(";")).contains(android.provider.Settings.AUTHORITY)){
                    Integer count=HostContext.hostBootCount();
                    if(count!=null)android.provider.Settings.Global.putInt(context.getContentResolver(),android.provider.Settings.Global.BOOT_COUNT,count);
                }
            }catch(RuntimeException|Error failure){synchronized(pending){pending.remove(info.packageName);}throw failure;}
        };
        if(Looper.myLooper()==context.getMainLooper())install.run();
        else if(!new Handler(context.getMainLooper()).post(install)){synchronized(pending){pending.remove(info.packageName);}throw new IllegalStateException("Android system provider owner has stopped");}
    }
}
