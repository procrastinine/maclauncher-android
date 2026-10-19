package maclauncher.android.framework;

import android.content.pm.ApplicationInfo;
import java.nio.file.Path;
import java.util.Objects;

/** Inspected package metadata for bootstrapping Android's original ContextImpl. */
public final class HostContext {
    private HostContext(){}
    private static ClassLoader hostClassLoader;
    private static Integer bootCount;
    public static void hostBootCount(int value){if(value<0)throw new IllegalArgumentException("Invalid Android boot count");bootCount=value;}
    static Integer hostBootCount(){return bootCount;}
    public static void hostClassLoader(ClassLoader value){hostClassLoader=Objects.requireNonNull(value);}
    static ClassLoader hostClassLoader(){return hostClassLoader==null?HostContext.class.getClassLoader():hostClassLoader;}
    private static Path directory(String value){try{return java.nio.file.Files.createDirectories(Path.of(value),java.nio.file.attribute.PosixFilePermissions.asFileAttribute(java.nio.file.attribute.PosixFilePermissions.fromString("rwx------"))).toRealPath();}catch(java.io.IOException failure){throw new java.io.UncheckedIOException(failure);}}
    public static void hostFilesDirectory(String value){directory(value);}
    public static void hostCacheDirectory(String value){directory(value);}
    public static Path hostDataRoot(){if(application==null||application.dataDir==null)throw new IllegalStateException("Application data directory is not configured");return directory(application.dataDir);}
    public static void hostApplicationTheme(int value){if(application==null)throw new IllegalStateException("APK application metadata is not configured");application.theme=value;}
    public static ApplicationInfo hostApplicationInfoSnapshot(){if(application==null)throw new IllegalStateException("APK application metadata is not configured");var copy=new ApplicationInfo(application);FrameworkResources.bindPackageSources(copy);return copy;}
    /** Preserve the complete parsed ApplicationInfo; only host-owned installation paths differ. */
    static void adoptApplicationInfo(ApplicationInfo parsed,android.os.Bundle metadata){
        if(application==null||!Objects.equals(packageName,parsed.packageName))throw new IllegalStateException("Application installation identity differs");
        ApplicationInfo next=new ApplicationInfo(parsed);next.metaData=metadata==null?null:metadata.deepCopy();
        next.uid=application.uid;next.dataDir=application.dataDir;next.credentialProtectedDataDir=application.credentialProtectedDataDir;next.deviceProtectedDataDir=application.deviceProtectedDataDir;next.nativeLibraryDir=application.nativeLibraryDir;next.secondaryNativeLibraryDir=application.secondaryNativeLibraryDir;
        if(application.primaryCpuAbi!=null){next.primaryCpuAbi=application.primaryCpuAbi;next.secondaryCpuAbi=application.secondaryCpuAbi;}
        application=next;applicationMetadataInspected=true;
        hostProcessName(next.processName==null?packageName:next.processName);
    }
    static void bindHostPaths(ApplicationInfo value){
        value.uid=application.uid;value.dataDir=application.dataDir;value.credentialProtectedDataDir=application.credentialProtectedDataDir;value.deviceProtectedDataDir=application.deviceProtectedDataDir;value.nativeLibraryDir=application.nativeLibraryDir;value.secondaryNativeLibraryDir=application.secondaryNativeLibraryDir;FrameworkResources.bindPackageSources(value);
        value.primaryCpuAbi=application.primaryCpuAbi;value.secondaryCpuAbi=application.secondaryCpuAbi;
    }
    private static String packageName,processName;
    /** Package principal configured from the private installation, independent of Context overrides. */
    public static String hostPackageName(){if(packageName==null)throw new IllegalStateException("APK process identity has not been configured");return packageName;}
    public static String hostProcessName(){if(packageName==null)throw new IllegalStateException("APK process identity has not been configured");return processName==null?packageName:processName;}
    public static void hostValidateProcessName(String value){if(value==null||!value.matches("[A-Za-z_][\\w]*(?:\\.[A-Za-z_][\\w]*)*(?::[A-Za-z_][\\w]*(?:\\.[A-Za-z_][\\w]*)*)?"))throw new IllegalArgumentException("Invalid Android process name");}
    public static void hostProcessName(String value){hostValidateProcessName(value);processName=value;android.os.Process.setArgV0(value);}
    public static void hostApplicationProcessName(String value){hostValidateProcessName(value);if(application==null)throw new IllegalStateException("APK application metadata is not configured");application.processName=value;}
    public static void hostPackage(String name,String version,int code){
        if(name==null||name.isBlank()||version==null)throw new IllegalArgumentException("Invalid inspected APK metadata");
        packageName=name;processName=name;android.os.Process.setArgV0(name);HostPackages.configure(name,version,code);
    }
    private static android.content.pm.ApplicationInfo application;
    private static boolean applicationMetadataInspected;
    public static void hostApplicationIdentity(int uid,int icon){
        if(application==null)throw new IllegalStateException("APK application metadata is not configured");
        if(uid<0)throw new IllegalArgumentException("Invalid host user ID");
        application.uid=uid;application.icon=icon;
    }
    public static void hostApplicationNativeAbi(String abi){
        if(application==null)throw new IllegalStateException("APK application metadata is not configured");
        application.primaryCpuAbi=abi;application.secondaryCpuAbi=null;
    }
    public static void hostApplicationMetaData(android.os.Bundle value){if(application==null)throw new IllegalStateException("APK application metadata is not configured");application.metaData=value==null?null:new android.os.Bundle(value);applicationMetadataInspected=true;}
    public static boolean hostApplicationMetaDataInspected(){return applicationMetadataInspected;}
    public static void hostApplicationInfo(boolean debug,String source,String data,String natives,int targetSdk){
        hostApplicationInfo(android.content.pm.ApplicationInfo.FLAG_HAS_CODE|(debug?android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE:0),source,data,natives,targetSdk);
    }
    public static void hostApplicationInfo(int flags,String source,String data,String natives,int targetSdk){
        applicationMetadataInspected=false;
        application=new android.content.pm.ApplicationInfo();application.packageName=packageName;application.processName=packageName;application.flags=flags;application.sourceDir=source;application.publicSourceDir=source;application.dataDir=data;application.nativeLibraryDir=natives;application.targetSdkVersion=targetSdk;application.credentialProtectedDataDir=data;application.deviceProtectedDataDir=directory(java.nio.file.Path.of(data,"device_protected").toString()).toString();
    }

    public static void hostShutdown(){hostClassLoader=null;bootCount=null;application=null;packageName=null;processName=null;applicationMetadataInspected=false;}
}
