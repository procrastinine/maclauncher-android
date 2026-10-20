package maclauncher.android.framework;

import android.app.ActivityThread;
import android.content.Context;
import android.content.pm.ProviderInfo;
import android.os.*;
import java.io.IOException;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.*;

/** Original supplied APK components share their manifest-declared system owner. */
final class HostPlatformPackages {
    private static final Map<String,Installation> pending=new HashMap<>();
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
    private static final class Installation implements Runnable {
        private final Context context;
        private final ProviderInfo info;
        private final AtomicBoolean started=new AtomicBoolean();
        private final CompletableFuture<Void> completion=new CompletableFuture<>();
        private volatile Thread owner;
        Installation(Context context,ProviderInfo info){this.context=context;this.info=info;}
        @Override public void run(){
            if(!started.compareAndSet(false,true))return;
            owner=Thread.currentThread();
            try{
                var thread=Objects.requireNonNull(ActivityThread.currentActivityThread());
                thread.installSystemProviders(HostPackages.platformProviders(info.packageName));
                // SystemServer normally records this value during boot. This
                // profile's authenticated boot owner supplies the same input.
                if(Arrays.asList(info.authority.split(";")).contains(android.provider.Settings.AUTHORITY)){
                    Integer count=HostContext.hostBootCount();
                    if(count!=null)android.provider.Settings.Global.putInt(context.getContentResolver(),android.provider.Settings.Global.BOOT_COUNT,count);
                }
                completion.complete(null);
            }catch(RuntimeException|Error failure){retire();completion.completeExceptionally(failure);throw failure;}
            finally{owner=null;}
        }
        private void retire(){synchronized(pending){pending.remove(info.packageName,this);}}
        void rejected(){
            if(!started.compareAndSet(false,true))return;
            retire();completion.completeExceptionally(new IllegalStateException("Android system provider owner has stopped"));
        }
        void await(){
            try{completion.get();}
            catch(InterruptedException failure){Thread.currentThread().interrupt();throw new IllegalStateException("Android system provider initialization was interrupted",failure);}
            catch(ExecutionException failure){Throwable cause=failure.getCause();if(cause instanceof RuntimeException error)throw error;if(cause instanceof Error error)throw error;throw new IllegalStateException("Android system provider initialization failed",cause);}
        }
    }
    /** A local synchronous acquisition cannot return an unfinished attachment.
     * Reentrant lookups from the original provider's own onCreate still belong
     * to that same main-thread installation and must not wait on themselves. */
    static void demand(Context context,ProviderInfo info){install(context,info,true);}
    /** Remote holders retain Android's original asynchronous publish callback. */
    static void request(Context context,ProviderInfo info){install(context,info,false);}
    private static void install(Context context,ProviderInfo info,boolean wait){
        if(!owns(info))return;
        Installation installation;boolean created;
        synchronized(pending){
            installation=pending.get(info.packageName);created=installation==null;
            if(created){installation=new Installation(context,info);pending.put(info.packageName,installation);}
        }
        if(installation.owner==Thread.currentThread())return;
        if(Looper.myLooper()==context.getMainLooper())installation.run();
        else if(created&&!new Handler(context.getMainLooper()).post(installation)){installation.rejected();installation.await();}
        if(wait)installation.await();
    }
}
