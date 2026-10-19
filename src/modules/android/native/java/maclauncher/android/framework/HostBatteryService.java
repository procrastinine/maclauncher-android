package maclauncher.android.framework;

import android.content.Context;
import android.content.Intent;
import android.os.*;
import com.android.server.LocalServices;
import com.android.internal.app.IBatteryStats;
import java.util.*;
import maclauncher.android.bridge.LocalBroadcasts;

/** App-scoped virtual battery beneath Android's original battery clients. */
public final class HostBatteryService implements AutoCloseable {
    private final Context context;
    private final Handler handler;
    private final Registrar registrar=new Registrar();
    private final Stats stats=new Stats();
    private final Internal internal=new Internal();
    private final Set<BatteryManagerInternal.ChargingPolicyChangeListener> policyListeners=new LinkedHashSet<>();
    private final Runnable refresh=this::refresh;
    private volatile Snapshot snapshot;
    private boolean closed;
    private int sequence;

    private record Snapshot(boolean present,int level,int plug,int status,int health,boolean low,
                            int voltage,int temperature,long current,long secondsToEmpty,long secondsToFull) {
        static Snapshot read(){
            return new Snapshot(true,100,BatteryManager.BATTERY_PLUGGED_AC,
                BatteryManager.BATTERY_STATUS_FULL,BatteryManager.BATTERY_HEALTH_GOOD,
                false,4000,250,0,-1,0);
        }
        boolean charging(){return status==BatteryManager.BATTERY_STATUS_CHARGING||status==BatteryManager.BATTERY_STATUS_FULL;}
        Snapshot withLevel(int next,int warningLevel){return new Snapshot(present,next,plug,status,health,plug==0&&next<=warningLevel,voltage,temperature,current,secondsToEmpty,secondsToFull);}
        Snapshot withPlug(int next){return new Snapshot(present,level,next,next==0?BatteryManager.BATTERY_STATUS_DISCHARGING:BatteryManager.BATTERY_STATUS_CHARGING,health,next==0&&low,voltage,temperature,current,secondsToEmpty,secondsToFull);}
    }

    HostBatteryService(Context context,Handler handler){
        this.context=context;this.handler=handler;snapshot=Snapshot.read();
        try{
            HostServices.register("batteryproperties",registrar,"android.os.IBatteryPropertiesRegistrar");
            HostServices.register("batterystats",stats,"com.android.internal.app.IBatteryStats");
            LocalServices.addService(BatteryManagerInternal.class,internal);
            publish(null,snapshot);
        }catch(Throwable failure){close();throw failure;}
    }
    private synchronized void requestRefresh(){if(!closed){handler.removeCallbacks(refresh);handler.post(refresh);}}
    private void refresh(){
        Snapshot current;synchronized(this){if(closed)return;current=snapshot;}
        publish(null,current);
    }
    public boolean isCharging(){return snapshot.charging();}
    public long chargeTimeRemainingMillis(){long seconds=snapshot.secondsToFull();return seconds<0?-1:seconds*1000;}
    public long dischargeTimeRemainingSeconds(){return snapshot.secondsToEmpty();}
    private void publish(Snapshot previous,Snapshot next){
        final int seq;
        synchronized(this){if(closed||snapshot!=next)return;seq=++sequence;}
        if(previous!=null){
            if((previous.plug()!=0)!=(next.plug()!=0))send(new Intent(next.plug()!=0?Intent.ACTION_POWER_CONNECTED:Intent.ACTION_POWER_DISCONNECTED).putExtra(BatteryManager.EXTRA_SEQUENCE,seq),false);
            if(previous.charging()!=next.charging())send(new Intent(next.charging()?BatteryManager.ACTION_CHARGING:BatteryManager.ACTION_DISCHARGING).putExtra(BatteryManager.EXTRA_SEQUENCE,seq),false);
            if(previous.low()!=next.low())send(new Intent(next.low()?Intent.ACTION_BATTERY_LOW:Intent.ACTION_BATTERY_OKAY).putExtra(BatteryManager.EXTRA_SEQUENCE,seq),false);
        }
        Intent changed=new Intent(Intent.ACTION_BATTERY_CHANGED)
            .addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY|Intent.FLAG_RECEIVER_REPLACE_PENDING)
            .putExtra(BatteryManager.EXTRA_SEQUENCE,seq).putExtra(BatteryManager.EXTRA_PRESENT,next.present())
            .putExtra(BatteryManager.EXTRA_LEVEL,next.level()).putExtra(BatteryManager.EXTRA_SCALE,100)
            .putExtra(BatteryManager.EXTRA_PLUGGED,next.plug()).putExtra(BatteryManager.EXTRA_STATUS,next.status())
            .putExtra(BatteryManager.EXTRA_HEALTH,next.health()).putExtra(BatteryManager.EXTRA_BATTERY_LOW,next.low())
            .putExtra(BatteryManager.EXTRA_VOLTAGE,next.voltage()).putExtra(BatteryManager.EXTRA_TEMPERATURE,next.temperature());
        send(changed,true);
    }
    private void send(Intent intent,boolean sticky){LocalBroadcasts.send(context,null,intent,null,null,0,null,null,null,null,null,false,sticky,context.getUserId());}

    private final class Registrar extends IBatteryPropertiesRegistrar.Default {
        private final Binder binder=new Binder();
        Registrar(){binder.attachInterface(this,"android.os.IBatteryPropertiesRegistrar");}
        @Override public IBinder asBinder(){return binder;}
        @Override public int getProperty(int id,BatteryProperty property){
            Snapshot value=snapshot;
            long result=switch(id){
                case BatteryManager.BATTERY_PROPERTY_CAPACITY -> value.present()?value.level():Long.MIN_VALUE;
                case BatteryManager.BATTERY_PROPERTY_STATUS -> value.status();
                case BatteryManager.BATTERY_PROPERTY_CURRENT_NOW -> value.current();
                default -> Long.MIN_VALUE;
            };
            property.setLong(result);
            return result==Long.MIN_VALUE?-1:0;
        }
        @Override public void scheduleUpdate(){requestRefresh();}
    }
    private final class Stats extends IBatteryStats.Default {
        private final Binder binder=new Binder();
        Stats(){binder.attachInterface(this,"com.android.internal.app.IBatteryStats");}
        @Override public IBinder asBinder(){return binder;}
        @Override public boolean isCharging(){return HostBatteryService.this.isCharging();}
        @Override public long computeBatteryTimeRemaining(){long seconds=dischargeTimeRemainingSeconds();return seconds<0?-1:seconds*1000;}
        @Override public long computeChargeTimeRemaining(){return chargeTimeRemainingMillis();}
        @Override public List<BatteryUsageStats> getBatteryUsageStats(List<BatteryUsageStatsQuery> queries){return List.of();}
    }
    private final class Internal extends BatteryManagerInternal {
        @Override public boolean isPowered(int plugs){Snapshot value=snapshot;return value.status()==BatteryManager.BATTERY_STATUS_UNKNOWN||(value.plug()&plugs)!=0;}
        @Override public int getPlugType(){return snapshot.plug();}
        @Override public int getBatteryLevel(){return snapshot.level();}
        @Override public int getBatteryHealth(){return snapshot.health();}
        // The virtual battery exposes no physical charge-capacity telemetry.
        @Override public int getBatteryChargeCounter(){return 0;}
        @Override public int getBatteryFullCharge(){return 0;}
        @Override public boolean getBatteryLevelLow(){return snapshot.low();}
        @Override public int getChargingPolicy(){return Integer.MIN_VALUE;}
        @Override public void registerChargingPolicyChangeListener(ChargingPolicyChangeListener listener){synchronized(HostBatteryService.this){if(!closed)policyListeners.add(Objects.requireNonNull(listener));}}
        @Override public int getInvalidCharger(){return 0;}
        @Override public void setChargerAcOnline(boolean online,boolean force){override(null,online?BatteryManager.BATTERY_PLUGGED_AC:0,force);}
        @Override public void setBatteryLevel(int level,boolean force){override(level,null,force);}
        @Override public void unplugBattery(boolean force){override(null,0,force);}
        @Override public void resetBattery(boolean force){Snapshot previous,next=Snapshot.read();synchronized(HostBatteryService.this){if(closed)return;previous=snapshot;snapshot=next;}if(force||!previous.equals(next))handler.post(()->publish(previous,next));}
        @Override public void suspendBatteryInput(){override(null,0,true);}
    }
    private void override(Integer level,Integer plug,boolean force){
        Snapshot previous,next;
        synchronized(this){if(closed)return;previous=snapshot;next=previous;if(level!=null)next=next.withLevel(level,context.getResources().getInteger(com.android.internal.R.integer.config_lowBatteryWarningLevel));if(plug!=null)next=next.withPlug(plug);snapshot=next;}
        if(force||!previous.equals(next)){Snapshot value=next;handler.post(()->publish(previous,value));}
    }
    @Override public void close(){
        synchronized(this){if(closed)return;closed=true;handler.removeCallbacks(refresh);policyListeners.clear();}
        HostServices.unregister("batteryproperties",registrar,"android.os.IBatteryPropertiesRegistrar");
        HostServices.unregister("batterystats",stats,"com.android.internal.app.IBatteryStats");
    }
}
