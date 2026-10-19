package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.os.*;
import java.util.*;

/** Application-local alarm delivery on the existing Android main message queue. */
public final class HostAlarmService extends IAlarmManager.Default implements AutoCloseable {
    private final Context context;
    private final Handler handler;
    private final Binder binder=new Binder();
    private final Map<Object,Alarm> alarms=new HashMap<>();
    private boolean closed;
    private HostAlarmService(Context context){this.context=context;handler=new Handler(context.getMainLooper());binder.attachInterface(this,"android.app.IAlarmManager");}
    public static void install(Context context){var service=new HostAlarmService(context);HostServices.register(Context.ALARM_SERVICE,service,"android.app.IAlarmManager");HostLooper.addShutdownListener(context.getMainLooper(),service::close);}
    @Override public IBinder asBinder(){return binder;}
    private static long now(int type){return type==AlarmManager.RTC||type==AlarmManager.RTC_WAKEUP?System.currentTimeMillis():SystemClock.elapsedRealtime();}
    private final class Alarm implements Runnable {
        final Object key;final String packageName;final int type;final long repeat;
        final PendingIntent operation;final IAlarmListener listener;final AlarmManager.AlarmClockInfo clock;
        long trigger;
        Alarm(Object key,String packageName,int type,long trigger,long repeat,PendingIntent operation,IAlarmListener listener,AlarmManager.AlarmClockInfo clock){this.key=key;this.packageName=packageName;this.type=type;this.trigger=trigger;this.repeat=repeat;this.operation=operation;this.listener=listener;this.clock=clock;}
        void schedule(){long delay=Math.max(0,trigger-now(type));handler.postDelayed(this,delay);}
        @Override public void run(){
            long missed;
            synchronized(HostAlarmService.this){if(closed||alarms.get(key)!=this)return;if(trigger>now(type)){schedule();return;}missed=repeat==0?1:1+Math.max(0,now(type)-trigger)/repeat;if(repeat==0)alarms.remove(key);}
            try{
                if(operation!=null)operation.send(context,0,new Intent().putExtra(Intent.EXTRA_ALARM_COUNT,(int)Math.min(Integer.MAX_VALUE,missed)));
                else listener.doAlarm(new IAlarmCompleteListener.Stub(){@Override public void alarmComplete(IBinder who){}});
            }catch(PendingIntent.CanceledException|RemoteException gone){synchronized(HostAlarmService.this){alarms.remove(key,this);}return;}
            synchronized(HostAlarmService.this){if(repeat>0&&!closed&&alarms.get(key)==this){long current=now(type);long intervals=Math.max(1,(current-trigger)/repeat+1);try{trigger=Math.addExact(trigger,Math.multiplyExact(intervals,repeat));schedule();}catch(ArithmeticException beyondClock){alarms.remove(key,this);}}}
        }
    }
    @Override public synchronized void set(String packageName,int type,long trigger,long window,long interval,int flags,PendingIntent operation,IAlarmListener listener,String tag,WorkSource work,AlarmManager.AlarmClockInfo clock){
        if(type<AlarmManager.RTC_WAKEUP||type>AlarmManager.ELAPSED_REALTIME)throw new IllegalArgumentException("Invalid alarm clock type");
        if((operation==null)==(listener==null))throw new IllegalArgumentException("Alarm requires one delivery target");
        if(interval<0)throw new IllegalArgumentException("Alarm interval must not be negative");
        if(closed)throw new IllegalStateException("Alarm service is closed");
        Object key=operation!=null?operation:Objects.requireNonNull(listener.asBinder());
        Alarm alarm=new Alarm(key,packageName,type,Math.max(0,trigger),interval,operation,listener,clock),previous=alarms.put(key,alarm);
        if(previous!=null)handler.removeCallbacks(previous);alarm.schedule();
    }
    @Override public synchronized void remove(PendingIntent operation,IAlarmListener listener){if(operation!=null)cancel(operation);if(listener!=null)cancel(listener.asBinder());}
    private void cancel(Object key){Alarm alarm=alarms.remove(key);if(alarm!=null)handler.removeCallbacks(alarm);}
    @Override public synchronized void removeAll(String packageName){for(var entry:List.copyOf(alarms.values()))if(Objects.equals(packageName,entry.packageName))cancel(entry.key);}
    @Override public synchronized long getNextWakeFromIdleTime(){return alarms.values().stream().filter(a->a.type==AlarmManager.RTC_WAKEUP||a.type==AlarmManager.ELAPSED_REALTIME_WAKEUP).mapToLong(a->SystemClock.elapsedRealtime()+Math.max(0,a.trigger-now(a.type))).min().orElse(0);}
    @Override public synchronized AlarmManager.AlarmClockInfo getNextAlarmClock(int user){return alarms.values().stream().map(a->a.clock).filter(Objects::nonNull).min(Comparator.comparingLong(AlarmManager.AlarmClockInfo::getTriggerTime)).orElse(null);}
    @Override public boolean canScheduleExactAlarms(String packageName){return context.getPackageName().equals(packageName);}
    @Override public boolean hasScheduleExactAlarm(String packageName,int user){return canScheduleExactAlarms(packageName);}
    @Override public void setTimeZone(String zone){TimeZone.setDefault(TimeZone.getTimeZone(java.time.ZoneId.of(zone)));}
    @Override public synchronized void close(){if(closed)return;closed=true;for(Alarm alarm:alarms.values())handler.removeCallbacks(alarm);alarms.clear();HostServices.unregister(Context.ALARM_SERVICE,this,"android.app.IAlarmManager");}
}
