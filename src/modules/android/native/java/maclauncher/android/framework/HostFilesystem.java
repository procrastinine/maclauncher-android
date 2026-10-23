package maclauncher.android.framework;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import org.json.*;

/** Actual filesystem locations supplied to the unchanged Android path and volume APIs. */
public final class HostFilesystem {
    private HostFilesystem(){}
    private record Mapping(Path android,Path host){}
    private record PathsState(Path runtime,Path partitions,Path data,Path files,Path cache,String packageName,Map<String,Path> environment,List<Mapping> mappings){}
    private static volatile PathsState paths;
    private static List<Mapping> packageMappings=List.of();
    private static final JSONObject NAMESPACE=namespace();
    private static JSONObject namespace(){
        try(var input=HostFilesystem.class.getResourceAsStream("storage-namespace.json")){
            if(input==null)throw new IOException("Android storage namespace is absent");
            return new JSONObject(new String(input.readAllBytes(),StandardCharsets.UTF_8));
        }catch(IOException|JSONException failure){throw new ExceptionInInitializerError(failure);}
    }

    public static synchronized void configure(Path runtime,Path data,Path files,Path cache,String packageName)throws IOException{
        runtime=runtime.toRealPath();
        configure(runtime,runtime.resolve("android-root"),data,files,cache,packageName);
    }
    /** ART already resolves these paths through its original Android VFS. */
    public static synchronized void configureAndroid(Path data,Path files,Path cache,String packageName)throws IOException{
        configure(Path.of("/"),Path.of("/"),data,files,cache,packageName);
    }
    private static void configure(Path runtime,Path partitions,Path data,Path files,Path cache,String packageName)throws IOException{
        runtime=runtime.toRealPath();partitions=partitions.toAbsolutePath().normalize();data=data.toRealPath();files=files.toRealPath();cache=cache.toRealPath();Objects.requireNonNull(packageName);
        if(packageName.length()>255||!packageName.matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*"))throw new IllegalArgumentException("Invalid Android package name");
        if(!Files.isDirectory(runtime)||!Files.isDirectory(data)||!files.startsWith(data)||!cache.startsWith(data)||!Files.isDirectory(files)||!Files.isDirectory(cache))throw new IOException("Android paths must remain in application storage");
        if(paths!=null){if(paths.runtime.equals(runtime)&&paths.partitions.equals(partitions)&&paths.data.equals(data)&&paths.files.equals(files)&&paths.cache.equals(cache)&&paths.packageName.equals(packageName))return;throw new IllegalStateException("Android filesystem is already configured");}
        var environment=new LinkedHashMap<String,Path>();var mappings=new ArrayList<Mapping>();
        // These partition paths are inside the read-only runtime scope. A missing
        // Android partition remains absent; no synthetic filesystem is created.
        var values=Map.of("partitions",partitions.toString(),"data",data.toString(),"files",files.toString(),"cache",cache.toString(),"package",packageName);
        try{
            JSONObject roots=NAMESPACE.getJSONObject("partitions"),variables=NAMESPACE.getJSONObject("environment");
            for(var keys=roots.keys();keys.hasNext();){String key=keys.next(),name=roots.getString(key);Path value=partitions.resolve(name);environment.put(key,value);mappings.add(new Mapping(Path.of("/"+name),value));}
            for(var keys=variables.keys();keys.hasNext();){String key=keys.next();environment.put(key,expand(variables.getString(key),values));}
            JSONArray aliases=NAMESPACE.getJSONArray("mappings");for(int i=0;i<aliases.length();i++){JSONArray pair=aliases.getJSONArray(i);mappings.add(new Mapping(expand(pair.getString(0),values),expand(pair.getString(1),values)));}
            for(String group:List.of("data","storage")){
                JSONArray prefixes=NAMESPACE.getJSONArray(group+"Prefixes"),pairs=NAMESPACE.getJSONArray(group+"Aliases");
                for(int i=0;i<prefixes.length();i++)for(int j=0;j<pairs.length();j++){JSONArray pair=pairs.getJSONArray(j);mappings.add(new Mapping(expand(prefixes.getString(i),values).resolve(expand(pair.getString(0),values)),expand(pair.getString(1),values)));}
            }
        }catch(JSONException failure){throw new IOException("Android storage namespace is invalid",failure);}
        mappings.sort(Comparator.comparingInt((Mapping value)->value.android.getNameCount()).reversed());
        paths=new PathsState(runtime,partitions,data,files,cache,packageName,Map.copyOf(environment),List.copyOf(mappings));
    }
    private static Path expand(String value,Map<String,String> variables){for(var entry:variables.entrySet())value=value.replace("{"+entry.getKey()+"}",entry.getValue());return Path.of(value).normalize();}
    private static PathsState current(){var value=paths;if(value==null)throw new IllegalStateException("Android filesystem has not been configured");return value;}
    /** The Mac filesystem has no Android SELinux labels to restore. */
    public static boolean restoreSecurityContext(File path){Objects.requireNonNull(path);return false;}
    public static String environment(String name){Objects.requireNonNull(name);PathsState configured=paths;Path value=configured==null?null:configured.environment.get(name);return value==null?System.getenv(name):value.toString();}
    public static Map<String,String> environment(){var values=new HashMap<>(System.getenv());PathsState configured=paths;if(configured!=null)configured.environment.forEach((key,value)->values.put(key,value.toString()));return Collections.unmodifiableMap(values);}
    static Path storageRoot(){return current().files;}
    static Path dataRoot(){return current().data;}
    static Path runtimeRoot(){return current().runtime;}
    static String packageName(){return current().packageName;}
    /** Native and Java file operations consume the same installed namespace. */
    public static synchronized void installNativeMappings(List<Path> sourceApks,Path assets)throws IOException{
        installNativeMappings(sourceApks,assets,null);
    }
    /** A fused module remains in its containing APK; no second APK is installed. */
    public static synchronized void installNativeMappings(List<Path> sourceApks,Path assets,android.content.pm.ApplicationInfo info)throws IOException{
        var current=current();Path root=assets.toRealPath();var mappings=new ArrayList<>(current.mappings);mappings.removeAll(packageMappings);
        if(info!=null&&info.packageName!=null&&!current.packageName.equals(info.packageName))
            throw new IllegalArgumentException("Installed code namespace differs from the current package");
        var archives=new ArrayList<Mapping>();
        for(Path source:sourceApks){
            Path original=source.toAbsolutePath().normalize(),actual=source.toRealPath();
            archives.add(new Mapping(original,actual));
            archives.add(new Mapping(original.resolve("assets"),root));
            if(!original.equals(actual))archives.add(new Mapping(actual.resolve("assets"),root));
        }
        // Linux memory-map names belong to the installed Android package;
        // physical directories remain valid I/O and native-load identities.
        var nativeAliases=new LinkedHashMap<Path,Path>();
        if(info!=null){
            String[] directories={info.nativeLibraryDir,info.secondaryNativeLibraryDir};
            String[] abis={info.primaryCpuAbi,info.secondaryCpuAbi};
            for(int i=0;i<directories.length;i++){
                if(directories[i]==null)continue;
                Path original=Path.of(directories[i]).toAbsolutePath().normalize(),actual=original.toRealPath();
                archives.add(new Mapping(original,original));
                String isa=nativeInstructionSet(abis[i],actual);
                if(isa==null)continue;
                Path alias=Path.of("/data/app",current.packageName,"lib",isa);
                Path previous=nativeAliases.putIfAbsent(alias,actual);
                if(previous!=null&&!previous.equals(actual))throw new IOException("Installed native directories share an instruction-set path");
            }
        }
        nativeAliases.forEach((alias,actual)->archives.add(new Mapping(alias,actual)));
        for(var alias:fusedArchivePaths(sourceApks,info).entrySet()){
            archives.add(new Mapping(alias.getKey(),alias.getValue()));
            archives.add(new Mapping(alias.getKey().resolve("assets"),root));
        }
        // Package code, archive aliases and assets share one replaceable owner.
        mappings.addAll(archives);
        mappings.sort(Comparator.comparingInt((Mapping value)->value.android.getNameCount()).reversed());
        byte[][] prefixes=new byte[mappings.size()][],targets=new byte[mappings.size()][];
        for(int i=0;i<mappings.size();i++){Mapping mapping=mappings.get(i);prefixes[i]=mapping.android.toString().getBytes(StandardCharsets.UTF_8);targets[i]=mapping.host.toString().getBytes(StandardCharsets.UTF_8);}
        installNativePaths(prefixes,targets);
        paths=new PathsState(current.runtime,current.partitions,current.data,current.files,current.cache,current.packageName,current.environment,List.copyOf(mappings));
        packageMappings=List.copyOf(archives);
    }
    private static String nativeInstructionSet(String abi,Path directory){
        if(abi!=null)return dalvik.system.VMRuntime.getInstructionSet(abi);
        String name=directory.getFileName().toString();
        try{return dalvik.system.VMRuntime.getInstructionSet(name);}
        catch(IllegalArgumentException unknownDirectory){
            for(String supported:android.os.Build.SUPPORTED_ABIS){
                String isa=dalvik.system.VMRuntime.getInstructionSet(supported);
                if(isa.equals(name))return isa;
            }
            // Older callers can supply an unclassified secondary directory.
            // Its physical identity remains usable without inventing an ABI.
            return null;
        }
    }
    /** Fused module names resolve to original APKs in either VM's filesystem. */
    public static Map<Path,Path> fusedArchivePaths(List<Path> sourceApks,android.content.pm.ApplicationInfo info)throws IOException{
        var aliases=new LinkedHashMap<Path,Path>();
        if(info!=null&&info.metaData!=null&&!sourceApks.isEmpty()){
            String fused=info.metaData.getString("com.android.dynamic.apk.fused.modules");
            if(fused!=null){
                Path base=sourceApks.get(0).toRealPath();
                for(String raw:fused.split(",")){
                    String module=raw.trim();
                    if(module.isEmpty()||module.equals("base")||module.indexOf('/')>=0||module.indexOf('\\')>=0||module.indexOf('\0')>=0)continue;
                    Path target=base;
                    // The original parsed split catalog takes precedence over
                    // fused metadata when both describe the same module.
                    if(info.splitNames!=null&&info.splitSourceDirs!=null)
                        for(int i=0;i<Math.min(info.splitNames.length,info.splitSourceDirs.length);i++)
                            if(module.equals(info.splitNames[i])){target=Path.of(info.splitSourceDirs[i]).toRealPath();break;}
                    Path alias=base.resolveSibling("split_"+module+".apk");
                    if(Files.exists(alias))continue;
                    aliases.put(alias,target);
                }
            }
        }
        return Collections.unmodifiableMap(aliases);
    }
    private static native void installNativePaths(byte[][] prefixes,byte[][] targets);
    public static String mapPath(String value){
        Path input=Path.of(Objects.requireNonNull(value));if(!input.isAbsolute())return value;
        input=input.normalize();PathsState configured=paths;
        if(configured==null){
            // Original framework components can initialize before an app Context.
            // Only installed read-only partitions exist at that point.
            try{JSONObject partitions=NAMESPACE.getJSONObject("partitions");for(var keys=partitions.keys();keys.hasNext();){String name=partitions.getString(keys.next());Path prefix=Path.of("/"+name);if(input.startsWith(prefix))return JvmNativeMemory.runtimeDirectory().resolve("android-root").resolve(name).resolve(prefix.relativize(input)).toString();}}catch(JSONException failure){throw new IllegalStateException(failure);}
            return value;
        }
        for(Mapping mapping:configured.mappings)if(input.startsWith(mapping.android))return mapping.host.resolve(mapping.android.relativize(input)).toString();
        return value;
    }
    public static File file(File value){return value==null?null:new File(mapPath(value.getPath()));}
    /** Preserve directory-prefix semantics while using the common installed namespace. */
    public static String directoryPath(String value){String mapped=mapPath(value);return value.endsWith("/")&&!mapped.endsWith("/")?mapped+"/":mapped;}
    /** Shared by actual libcore filesystem JNI; never broadens sandbox permissions. */
    public static byte[] pathBytes(String value){return mapPath(value).getBytes(StandardCharsets.UTF_8);}
}
