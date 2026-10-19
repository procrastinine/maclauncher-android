package maclauncher.android.framework;

import android.Manifest;
import android.app.ActivityManagerInternal;
import android.app.trust.ITrustListener;
import android.app.trust.ITrustManager;
import android.content.Context;
import android.hardware.biometrics.BiometricSourceType;
import android.os.*;
import com.android.internal.policy.IDeviceLockedStateListener;
import java.util.List;
import java.util.Objects;

/** Android trust service for the profile's unsecured desktop device. Android
 * has no configured credential, trust agent or biometric unlock source here;
 * Mac credentials and session locking are not Android authentication state. */
public final class HostTrustService extends ITrustManager.Stub implements AutoCloseable {
    private final Context context;
    private final Handler handler;
    private final RemoteCallbackList<ITrustListener> trustListeners=new RemoteCallbackList<>();
    private final RemoteCallbackList<IDeviceLockedStateListener> lockedListeners=new RemoteCallbackList<>();
    private volatile boolean closed;

    public HostTrustService(Context context){
        super(PermissionEnforcer.fromContext(context));
        if(android.os.Process.myUid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("Trust service requires the Android system owner");
        this.context=Objects.requireNonNull(context);handler=new Handler(context.getMainLooper());
        ServiceManager.addService(Context.TRUST_SERVICE,this);
    }
    private void active(){if(closed)throw new IllegalStateException("Android trust service is closed");}
    private void permission(String name){active();context.enforceCallingOrSelfPermission(name,"Android trust operation");}
    private int user(int id){active();return HostActivityManager.hostHandleIncomingUser(null,Binder.getCallingPid(),Binder.getCallingUid(),id,false,ActivityManagerInternal.ALLOW_FULL_ONLY,"Android trust operation",null);}
    private void report(){permission(Manifest.permission.ACCESS_KEYGUARD_SECURE_STORAGE);}
    private void post(Runnable task){active();if(!handler.post(()->{if(!closed)task.run();}))throw new IllegalStateException("Android trust service Looper is closed");}

    // Original trust reports notify active agents. With no Android agents or
    // separate secure profile challenge, they cannot grant or revoke a lock.
    @Override public void reportUnlockAttempt(boolean successful,int userId){report();}
    @Override public void reportUserRequestedUnlock(int userId,boolean dismissKeyguard){report();}
    @Override public void reportUserMayRequestUnlock(int userId){report();}
    @Override public void reportUnlockLockout(int timeoutMs,int userId){report();}
    @Override public void reportKeyguardShowingChanged(){permission(Manifest.permission.ACCESS_KEYGUARD_SECURE_STORAGE);}
    @Override public void setDeviceLockedForUser(int userId,boolean locked){report();}
    @Override public void unlockedByBiometricForUser(int userId,BiometricSourceType source){report();}
    @Override public void clearAllBiometricRecognized(BiometricSourceType target,int unlockedUser){report();}
    @Override public boolean isDeviceLocked(int userId,int deviceId){active();return deviceId==Context.DEVICE_ID_DEFAULT&&user(userId)!=context.getUserId();}
    @Override public boolean isDeviceSecure(int userId,int deviceId){active();if(deviceId==Context.DEVICE_ID_DEFAULT)user(userId);return false;}
    @Override public boolean isTrustUsuallyManaged(int userId){permission(Manifest.permission.TRUST_LISTENER);return false;}
    @Override public boolean isActiveUnlockRunning(int userId){active();return false;}
    @Override public boolean isInSignificantPlace(){permission(Manifest.permission.ACCESS_FINE_LOCATION);return false;}

    @Override public void registerTrustListener(ITrustListener listener){
        permission(Manifest.permission.TRUST_LISTENER);Objects.requireNonNull(listener);
        post(()->{
            int count=trustListeners.beginBroadcast();boolean registered=false;
            try{for(int i=0;i<count;i++)if(trustListeners.getBroadcastItem(i).asBinder().equals(listener.asBinder())){registered=true;break;}}
            finally{trustListeners.finishBroadcast();}
            if(registered)return;
            if(!trustListeners.register(listener))return;
            try{
                listener.onIsActiveUnlockRunningChanged(false,context.getUserId());
                listener.onTrustManagedChanged(false,context.getUserId());
                listener.onTrustChanged(false,false,context.getUserId(),0,List.of());
            }catch(RemoteException gone){trustListeners.unregister(listener);}
        });
    }
    @Override public void unregisterTrustListener(ITrustListener listener){permission(Manifest.permission.TRUST_LISTENER);Objects.requireNonNull(listener);post(()->trustListeners.unregister(listener));}
    @Override public void reportEnabledTrustAgentsChanged(int userId){
        report();
        post(()->{
            int count=trustListeners.beginBroadcast();
            try{for(int i=0;i<count;i++)try{trustListeners.getBroadcastItem(i).onEnabledTrustAgentsChanged(userId);}catch(RemoteException gone){/* Original Binder retires dead callbacks. */}}
            finally{trustListeners.finishBroadcast();}
        });
    }
    @Override public void registerDeviceLockedStateListener(IDeviceLockedStateListener listener,int deviceId){
        permission(Manifest.permission.SUBSCRIBE_TO_KEYGUARD_LOCKED_STATE);Objects.requireNonNull(listener);
        int user=UserHandle.getUserId(Binder.getCallingUid());
        if(deviceId==Context.DEVICE_ID_DEFAULT)post(()->lockedListeners.register(listener,user));
    }
    @Override public void unregisterDeviceLockedStateListener(IDeviceLockedStateListener listener){permission(Manifest.permission.SUBSCRIBE_TO_KEYGUARD_LOCKED_STATE);Objects.requireNonNull(listener);post(()->lockedListeners.unregister(listener));}
    @Override public void close(){
        if(closed)return;closed=true;
        HostServices.unregister(Context.TRUST_SERVICE,this,ITrustManager.class.getName());
        handler.removeCallbacksAndMessages(null);trustListeners.kill();lockedListeners.kill();
    }
}
