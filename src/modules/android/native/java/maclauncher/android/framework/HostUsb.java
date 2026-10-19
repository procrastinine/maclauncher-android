package maclauncher.android.framework;

import android.content.Context;
import android.hardware.usb.IUsbManager;
import android.os.FileObserver;
import android.os.Process;
import android.os.ServiceManager;
import android.util.SparseArray;
import com.android.server.usb.UsbService;

/** Lifetime and publication of the complete original USB service. Hardware
 * discovery remains selected by the original service's system-feature checks. */
public final class HostUsb implements AutoCloseable {
    private final UsbService service;
    private boolean closed;
    public HostUsb(Context context) {
        if(Process.myUid()!=Process.SYSTEM_UID)throw new SecurityException("USB services require the Android system owner");
        service=new UsbService(context);
        service.systemReady();
        ServiceManager.addService(Context.USB_SERVICE,service);
    }
    public void ready(){
        // Upstream's boot hook registers port-lockdown callbacks. A runtime
        // without a USB transport has no port manager or port state to lock.
        // When one is supplied, run the original hook and require its original
        // LockSettings dependency rather than substituting security policy.
        if(HostUiAccess.get(service,UsbService.class,"mPortManager")!=null)service.bootCompleted();
        service.onUnlockUser(0);
    }
    @Override public void close(){
        if(closed)return;closed=true;
        HostServices.unregister(Context.USB_SERVICE,service,IUsbManager.class.getName());
        Object settings=HostUiAccess.get(service,UsbService.class,"mSettingsManager");
        SparseArray<?> groups=(SparseArray<?>)HostUiAccess.get(settings,settings.getClass(),"mSettingsByProfileGroup");
        for(int i=0;i<groups.size();i++)HostUiAccess.call(HostUiAccess.method(groups.valueAt(i).getClass(),"unregisterReceivers"),groups.valueAt(i));
        Object audio=HostUiAccess.get(service,UsbService.class,"mAlsaManager");
        ((FileObserver)HostUiAccess.get(audio,audio.getClass(),"mAlsaObserver")).stopWatching();
    }
}
