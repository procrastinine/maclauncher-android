package maclauncher.android.framework;

import android.content.Context;
import android.content.pm.*;
import android.os.*;
import java.util.Objects;

/** Native package-manager AIDL over the same parsed installations and policy
 * used by Android's Java package manager. No package records are copied. */
public final class HostNativePackages extends IPackageManagerNative.Stub implements AutoCloseable {
    private static final String SERVICE="package_native";
    private final IPackageManager packages=HostPackages.service();
    private final String moduleMetadataPackage;
    private final RemoteCallbackList<IStagedApexObserver> stagedObservers=new RemoteCallbackList<>();
    private boolean closed;
    public HostNativePackages(Context context){
        if(android.os.Process.myUid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("Native package metadata requires the Android system owner");
        moduleMetadataPackage=Objects.requireNonNull(context).getResources().getString(com.android.internal.R.string.config_defaultModuleMetadataProvider);
        ServiceManager.addService(SERVICE,this);
    }
    private ApplicationInfo application(String name)throws RemoteException{return packages.getApplicationInfo(name,0,UserHandle.getCallingUserId());}
    private ApplicationInfo requiredApplication(String name)throws RemoteException{
        ApplicationInfo info=application(name);if(info==null)throw new RemoteException("Application metadata could not be found: "+name);return info;
    }
    @Override public String[] getNamesForUids(int[] uids)throws RemoteException{
        if(uids==null||uids.length==0)return null;
        String[] names=packages.getNamesForUids(uids);if(names==null)names=new String[uids.length];
        for(int i=0;i<names.length;i++)if(names[i]==null)names[i]="";return names;
    }
    @Override public int getPackageUid(String name,long flags,int user)throws RemoteException{return packages.getPackageUid(name,flags,user);}
    @Override public String getInstallerForPackage(String name)throws RemoteException{
        String installer=null;try{installer=packages.getInstallerPackageName(name);}catch(IllegalArgumentException missing){/* AOSP native contract represents absent installer metadata as an empty name. */}
        if(installer!=null&&!installer.isEmpty())return installer;
        ApplicationInfo info=application(name);return info!=null&&info.isSystemApp()?"preload":"";
    }
    @Override public long getVersionCodeForPackage(String name)throws RemoteException{
        try{PackageInfo info=packages.getPackageInfo(name,0,UserHandle.getCallingUserId());return info==null?0:info.getLongVersionCode();}
        catch(Exception unavailable){return 0;}
    }
    @Override public boolean[] isAudioPlaybackCaptureAllowed(String[] names)throws RemoteException{
        boolean[] allowed=new boolean[names.length];for(int i=0;i<names.length;i++){ApplicationInfo info=application(names[i]);allowed[i]=info!=null&&info.isAudioPlaybackCaptureAllowed();}return allowed;
    }
    @Override public int getLocationFlags(String name)throws RemoteException{
        ApplicationInfo info=requiredApplication(name);return(info.isSystemApp()?LOCATION_SYSTEM:0)|(info.isVendor()?LOCATION_VENDOR:0)|(info.isProduct()?LOCATION_PRODUCT:0);
    }
    @Override public int getTargetSdkVersionForPackage(String name)throws RemoteException{
        int sdk=packages.getTargetSdkVersion(name);if(sdk<0)throw new RemoteException("Target SDK could not be found: "+name);return sdk;
    }
    @Override public String getModuleMetadataPackageName(){return moduleMetadataPackage;}
    @Override public boolean hasSha256SigningCertificate(String name,byte[] certificate)throws RemoteException{return packages.hasSigningCertificate(name,certificate,PackageManager.CERT_INPUT_SHA256);}
    @Override public boolean isPackageDebuggable(String name)throws RemoteException{return(requiredApplication(name).flags&ApplicationInfo.FLAG_DEBUGGABLE)!=0;}
    @Override public boolean hasSystemFeature(String name,int version)throws RemoteException{return packages.hasSystemFeature(name,version);}
    // System image updates install a complete managed Mac runtime, rather than
    // staged Android APEX sessions. Listeners retain ordinary Binder lifetime;
    // the actual staged-session set is empty.
    @Override public void registerStagedApexObserver(IStagedApexObserver observer){if(observer!=null)stagedObservers.register(observer);}
    @Override public void unregisterStagedApexObserver(IStagedApexObserver observer){if(observer!=null)stagedObservers.unregister(observer);}
    @Override public StagedApexInfo[] getStagedApexInfos(){return new StagedApexInfo[0];}
    @Override public void close(){if(closed)return;closed=true;HostServices.unregister(SERVICE,this,IPackageManagerNative.class.getName());stagedObservers.kill();}
}
