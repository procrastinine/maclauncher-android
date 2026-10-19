package maclauncher.android.framework;

import android.content.Context;
import android.os.Process;
import android.os.ServiceManager;
import com.android.server.TelephonyRegistry;

/** Original telephony event registry. Android retains modem availability,
 * subscription validation, callback permissions and idle event state. */
public final class HostTelephonyRegistry implements AutoCloseable {
    private final TelephonyRegistry registry;
    private boolean closed;
    public HostTelephonyRegistry(Context context){
        if(Process.myUid()!=Process.SYSTEM_UID)throw new SecurityException("Telephony events require the Android system owner");
        registry=new TelephonyRegistry(context,new TelephonyRegistry.ConfigurationProvider());
        ServiceManager.addService("telephony.registry",registry);
        registry.systemRunning();
    }
    @Override public void close(){
        if(closed)return;closed=true;
        HostServices.unregister("telephony.registry",registry,"com.android.internal.telephony.ITelephonyRegistry");
        ((android.os.Handler)HostUiAccess.get(registry,TelephonyRegistry.class,"mHandler")).removeCallbacksAndMessages(null);
    }
}
