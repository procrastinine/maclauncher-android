package maclauncher.android.framework;

import android.content.Context;
import android.content.pm.UserInfo;
import android.hardware.ISensorPrivacyManager;
import android.os.IBinder;
import android.os.Process;
import com.android.server.SystemService;
import com.android.server.sensorprivacy.SensorPrivacyService;

/** Publication and process lifetime for the complete upstream sensor-privacy
 * service. Upstream retains state, persistence, permissions and listeners. */
public final class HostSensorPrivacy implements AutoCloseable {
    private final SensorPrivacyService service;
    private final ISensorPrivacyManager endpoint;
    private boolean closed;
    public HostSensorPrivacy(Context context){
        if(Process.myUid()!=Process.SYSTEM_UID)throw new SecurityException("Sensor privacy requires the Android system owner");
        service=new SensorPrivacyService(context);
        endpoint=(ISensorPrivacyManager)HostUiAccess.get(service,SensorPrivacyService.class,"mSensorPrivacyServiceImpl");
        try{
            service.onStart();
            UserInfo user=context.getSystemService(android.os.UserManager.class).getUserInfo(context.getUserId());
            service.onUserStarting(new SystemService.TargetUser(user));
        }catch(RuntimeException|Error failure){try{close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }
    public void ready(){
        service.onBootPhase(SystemService.PHASE_SYSTEM_SERVICES_READY);
        service.onBootPhase(SystemService.PHASE_ACTIVITY_MANAGER_READY);
    }
    ISensorPrivacyManager endpoint(){return endpoint;}
    @Override public void close(){
        if(closed)return;closed=true;
        HostServices.unregister(Context.SENSOR_PRIVACY_SERVICE,endpoint,ISensorPrivacyManager.class.getName());
        HostUserService.hostRemoveUserRestrictionsListener(null,(com.android.server.pm.UserManagerInternal.UserRestrictionsListener)endpoint);
        var token=(IBinder)HostUiAccess.get(service,SensorPrivacyService.class,"mAppOpsRestrictionToken");
        HostAppOps.retireGlobalRestrictions(token);
        ((android.os.Handler)HostUiAccess.get(endpoint,endpoint.getClass(),"mHandler")).removeCallbacksAndMessages(null);
    }
}
