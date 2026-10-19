package maclauncher.android.framework;

import android.content.Context;
import android.os.Build;
import android.os.IDeviceIdentifiersPolicyService;

/** No host hardware identifier is supplied to the Android device service. */
public final class HostIdentity extends IDeviceIdentifiersPolicyService.Default {
    private static final HostIdentity INSTANCE=new HostIdentity();
    private static boolean installed;
    private HostIdentity(){}
    public static synchronized void install(){
        if(installed)return;
        HostServices.register(Context.DEVICE_IDENTIFIERS_SERVICE,INSTANCE,"android.os.IDeviceIdentifiersPolicyService");
        installed=true;
    }
    @Override public String getSerial(){return Build.UNKNOWN;}
    @Override public String getSerialForPackage(String packageName,String featureId){return Build.UNKNOWN;}
}
