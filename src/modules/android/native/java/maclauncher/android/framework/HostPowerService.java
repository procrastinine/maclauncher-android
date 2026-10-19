package maclauncher.android.framework;

import android.content.Context;
import android.os.*;
import java.util.*;

/** App-owned virtual power state beneath the original Android power client. */
public final class HostPowerService extends IPowerManager.Default implements AutoCloseable {
    private final Map<IBinder,Lock> locks=new HashMap<>();
    private final Binder binder=new Binder();
    private final Handler handler;
    private final Thermal thermal=new Thermal();
    private final DeviceIdle deviceIdle;
    private final HostBatteryService battery;
    private IBinder published;
    private volatile boolean closed;
    private record Lock(IWakeLockCallback callback){}
    private HostPowerService(Context context){handler=new Handler(context.getMainLooper());binder.attachInterface(this,"android.os.IPowerManager");battery=new HostBatteryService(context,handler);deviceIdle=new DeviceIdle(context);}
    public static void install(Context context){
        var service=new HostPowerService(context);
        if(android.os.Process.myUid()==android.os.Process.SYSTEM_UID){
            service.published=HostServiceEndpoints.export(service,"android.os.IPowerManager",context);
            try{HostServices.manager().addService(Context.POWER_SERVICE,service.published,false,0);}
            catch(RemoteException failure){service.close();throw failure.rethrowFromSystemServer();}
        }else HostServices.register(Context.POWER_SERVICE,service,"android.os.IPowerManager");
        HostServices.register(Context.THERMAL_SERVICE,service.thermal,"android.os.IThermalService");
        HostServices.register(Context.DEVICE_IDLE_CONTROLLER,service.deviceIdle,"android.os.IDeviceIdleController");
        HostLooper.addShutdownListener(context.getMainLooper(),service::close);
    }
    @Override public IBinder asBinder(){return binder;}
    @Override public void acquireWakeLock(IBinder token,int flags,String tag,String packageName,WorkSource work,String history,int display,IWakeLockCallback callback){
        Objects.requireNonNull(token);PowerManager.validateWakeLockParameters(flags,tag);
        synchronized(this){
            if(closed)throw new IllegalStateException("Power service is closed");
            locks.put(token,new Lock(callback));
            notifyLater(callback,true);
        }
    }
    @Override public void acquireWakeLockWithUid(IBinder token,int flags,String tag,String packageName,int uid,int display,IWakeLockCallback callback){acquireWakeLock(token,flags,tag,packageName,null,null,display,callback);}
    @Override public void acquireWakeLockAsync(IBinder token,int flags,String tag,String packageName,WorkSource work,String history){acquireWakeLock(token,flags,tag,packageName,work,history,0,null);}
    @Override public synchronized void releaseWakeLock(IBinder token,int flags){Lock lock=locks.remove(token);if(lock!=null)notifyLater(lock.callback(),false);}
    @Override public void releaseWakeLockAsync(IBinder token,int flags){releaseWakeLock(token,flags);}
    @Override public synchronized void updateWakeLockCallback(IBinder token,IWakeLockCallback callback){Lock lock=locks.get(token);if(lock!=null){locks.put(token,new Lock(callback));notifyLater(callback,true);}}
    private void notifyLater(IWakeLockCallback callback,boolean held){if(callback!=null)handler.post(()->notify(callback,held));}
    private static void notify(IWakeLockCallback callback,boolean held){if(callback!=null)try{callback.onStateChanged(held);}catch(RemoteException ignored){}}
    @Override public boolean isWakeLockLevelSupported(int level){return level==PowerManager.PARTIAL_WAKE_LOCK||level==PowerManager.SCREEN_DIM_WAKE_LOCK||level==PowerManager.SCREEN_BRIGHT_WAKE_LOCK||level==PowerManager.FULL_WAKE_LOCK;}
    @Override public boolean isWakeLockLevelSupportedWithDisplayId(int level,int display){return display==0&&isWakeLockLevelSupported(level);}
    @Override public boolean isInteractive(){return !HostActivity.hostSleeping();}
    @Override public boolean isDisplayInteractive(int display){return display==0&&isInteractive();}
    @Override public boolean isPowerSaveMode(){return false;}
    @Override public boolean isBatterySaverSupported(){return true;}
    @Override public PowerSaveState getPowerSaveState(int type){boolean enabled=isPowerSaveMode();return new PowerSaveState.Builder().setBatterySaverEnabled(enabled).setGlobalBatterySaverEnabled(enabled).build();}
    @Override public BatterySaverPolicyConfig getFullPowerSavePolicy(){return new BatterySaverPolicyConfig.Builder().setAdvertiseIsEnabled(isPowerSaveMode()).build();}
    @Override public ParcelDuration getBatteryDischargePrediction(){long seconds=battery.dischargeTimeRemainingSeconds();return seconds<0?null:new ParcelDuration(java.time.Duration.ofSeconds(seconds));}
    @Override public boolean isExemptFromLowPowerStandby(){return true;}
    @Override public boolean isReasonAllowedInLowPowerStandby(int reason){return true;}
    @Override public boolean isFeatureAllowedInLowPowerStandby(String feature){return true;}
    @Override public List<LowPowerStandbyPortDescription> getActiveLowPowerStandbyPorts(){return List.of();}
    // Android activity never resets the Mac idle timer or wakes the computer.
    @Override public void userActivity(int display,long time,int event,int flags){}
    @Override public void wakeUp(long time,int reason,String details,String packageName){}
    @Override public void wakeUpWithDisplayId(long time,int reason,String details,String packageName,int display){}
    @Override public void close(){
        List<Lock> release;synchronized(this){if(closed)return;closed=true;release=List.copyOf(locks.values());locks.clear();}
        battery.close();thermal.close();for(Lock lock:release)notify(lock.callback(),false);
        if(published!=null)try{HostServices.manager().tryUnregisterService(Context.POWER_SERVICE,published);}catch(RemoteException failure){throw failure.rethrowFromSystemServer();}
        HostServices.unregister(Context.POWER_SERVICE,this,"android.os.IPowerManager");HostServices.unregister(Context.THERMAL_SERVICE,thermal,"android.os.IThermalService");HostServices.unregister(Context.DEVICE_IDLE_CONTROLLER,deviceIdle,"android.os.IDeviceIdleController");
    }
    /** Android Doze does not control these macOS processes. Consequently there
     * are no explicit system, user or temporary Doze exemptions. In particular,
     * unrestricted execution must not be misreported as whitelist membership.
     * The original managers retain validation and API behavior; privileged
     * exemption requests have no restriction to remove and create no timer or
     * second power policy beside this owner's logical wake locks. */
    private final class DeviceIdle extends IDeviceIdleController.Stub {
        private final Context context;
        DeviceIdle(Context context){this.context=context;}
        private void change(){context.enforceCallingOrSelfPermission(android.Manifest.permission.DEVICE_POWER,null);}
        private void temporary(int user)throws RemoteException{
            context.enforceCallingOrSelfPermission(android.Manifest.permission.CHANGE_DEVICE_IDLE_TEMP_WHITELIST,null);
            android.app.ActivityManager.getService().handleIncomingUser(Binder.getCallingPid(),Binder.getCallingUid(),user,false,false,"addPowerSaveTempWhitelistApp",null);
        }
        @Override public void addPowerSaveWhitelistApp(String name){change();}
        @Override public int addPowerSaveWhitelistApps(List<String> names){change();Objects.requireNonNull(names);return 0;}
        @Override public void removePowerSaveWhitelistApp(String name){change();}
        @Override public void removeSystemPowerWhitelistApp(String name){change();}
        @Override public void restoreSystemPowerWhitelistApp(String name){change();}
        @Override public String[] getRemovedSystemPowerWhitelistApps(){return new String[0];}
        @Override public String[] getSystemPowerWhitelistExceptIdle(){return new String[0];}
        @Override public String[] getSystemPowerWhitelist(){return new String[0];}
        @Override public String[] getUserPowerWhitelist(){return new String[0];}
        @Override public String[] getFullPowerWhitelistExceptIdle(){return new String[0];}
        @Override public String[] getFullPowerWhitelist(){return new String[0];}
        @Override public int[] getAppIdWhitelistExceptIdle(){return new int[0];}
        @Override public int[] getAppIdWhitelist(){return new int[0];}
        @Override public int[] getAppIdUserWhitelist(){return new int[0];}
        @Override public int[] getAppIdTempWhitelist(){return new int[0];}
        @Override public boolean isPowerSaveWhitelistExceptIdleApp(String name){return false;}
        @Override public boolean isPowerSaveWhitelistApp(String name){return false;}
        @Override public void addPowerSaveTempWhitelistApp(String name,long duration,int user,int reason,String detail)throws RemoteException{temporary(user);}
        @Override public long addPowerSaveTempWhitelistAppForMms(String name,int user,int reason,String detail)throws RemoteException{temporary(user);return 0;}
        @Override public long addPowerSaveTempWhitelistAppForSms(String name,int user,int reason,String detail)throws RemoteException{temporary(user);return 0;}
        @Override public long whitelistAppTemporarily(String name,int user,int reason,String detail)throws RemoteException{temporary(user);return 0;}
        @Override public void exitIdle(String reason){change();}
    }
    private final class Thermal extends IThermalService.Default {
        private final Binder binder=new Binder();
        private final Set<IThermalStatusListener> listeners=new LinkedHashSet<>();
        Thermal(){binder.attachInterface(this,"android.os.IThermalService");}
        @Override public IBinder asBinder(){return binder;}
        @Override public int getCurrentThermalStatus(){return PowerManager.THERMAL_STATUS_NONE;}
        @Override public synchronized boolean registerThermalStatusListener(IThermalStatusListener listener){Objects.requireNonNull(listener);if(closed||!listeners.add(listener))return false;try{listener.onStatusChange(getCurrentThermalStatus());}catch(RemoteException error){listeners.remove(listener);return false;}return true;}
        @Override public synchronized boolean unregisterThermalStatusListener(IThermalStatusListener listener){return listeners.remove(listener);}
        @Override public Temperature[] getCurrentTemperatures(){return new Temperature[0];}
        @Override public Temperature[] getCurrentTemperaturesWithType(int type){return getCurrentTemperatures();}
        @Override public CoolingDevice[] getCurrentCoolingDevices(){return new CoolingDevice[0];}
        @Override public CoolingDevice[] getCurrentCoolingDevicesWithType(int type){return getCurrentCoolingDevices();}
        @Override public float getThermalHeadroom(int seconds){return Float.NaN;}
        @Override public float[] getThermalHeadroomThresholds(){return new float[0];}
        synchronized void close(){listeners.clear();}
    }
}
