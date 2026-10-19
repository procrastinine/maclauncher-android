package maclauncher.android.framework;

import android.hardware.*;
import android.os.*;
import android.view.KeyEvent;
import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.*;

/** Manual motion source beneath the original SystemSensorManager and event queues. */
public final class HostSensors {
    private HostSensors(){}
    private static final int PERIOD_US=16_667;
    private static final int[] TYPES={Sensor.TYPE_ACCELEROMETER,Sensor.TYPE_GRAVITY,Sensor.TYPE_GYROSCOPE,Sensor.TYPE_LINEAR_ACCELERATION,Sensor.TYPE_ROTATION_VECTOR,Sensor.TYPE_GAME_ROTATION_VECTOR,Sensor.TYPE_ORIENTATION};
    private static final Class<?> BASE=HostUiAccess.type("android.hardware.SystemSensorManager$BaseEventQueue");
    private static final Method DISPATCH=HostUiAccess.method(BASE,"dispatchSensorEvent",int.class,float[].class,int.class,long.class);
    private static final Method FLUSH=HostUiAccess.method(BASE,"dispatchFlushCompleteEvent",int.class);
    private static final Method SET_TYPE=HostUiAccess.method(Sensor.class,"setType",int.class);
    private static final Set<Queue> queues=Collections.newSetFromMap(new IdentityHashMap<>());
    private static String control="off";
    private static final Set<Integer> keys=new HashSet<>();
    private static float targetPitch,targetRoll,pitch,roll,gyroPitch,gyroRoll;
    private static long updated;
    private static int publications;
    public static synchronized void configure(String source){
        if(!Set.of("off","keyboard","controller").contains(source))throw new IllegalArgumentException("Unknown manual tilt control");
        if(!queues.isEmpty()||publications!=0)throw new IllegalStateException("Manual tilt is already in use");control=source;reset();
    }
    public static synchronized void reset(){keys.clear();targetPitch=targetRoll=pitch=roll=gyroPitch=gyroRoll=0;updated=0;}
    /** Tilt keys are consumed only for the selected manual keyboard source. */
    public static synchronized boolean key(KeyEvent event){
        if(!control.equals("keyboard"))return false;
        if(!HostInput.keyboardControlsAllowed()){reset();return false;}
        int key=event.getKeyCode(),action=event.getAction();
        if(action!=KeyEvent.ACTION_DOWN&&action!=KeyEvent.ACTION_UP)return false;
        boolean axis=key==KeyEvent.KEYCODE_DPAD_LEFT||key==KeyEvent.KEYCODE_DPAD_RIGHT||key==KeyEvent.KEYCODE_DPAD_UP||key==KeyEvent.KEYCODE_DPAD_DOWN;
        boolean letter=key==KeyEvent.KEYCODE_Q||key==KeyEvent.KEYCODE_E||key==KeyEvent.KEYCODE_R;
        boolean shortcut=event.isMetaPressed()||event.isCtrlPressed();
        boolean selected=!shortcut&&(letter&&!event.isAltPressed()||event.isAltPressed()&&(axis||key==KeyEvent.KEYCODE_0));
        // Release a held tilt even if modifiers changed; native Cmd/Control
        // shortcuts still pass through without being consumed by sensors.
        if(action==KeyEvent.ACTION_UP&&keys.remove(key)){
            updateKeyboardTargets();return !shortcut;
        }
        if(!selected)return false;
        if(action==KeyEvent.ACTION_DOWN){if(key==KeyEvent.KEYCODE_R||key==KeyEvent.KEYCODE_0)reset();keys.add(key);}
        updateKeyboardTargets();return true;
    }
    private static void updateKeyboardTargets(){
        targetRoll=(keys.contains(KeyEvent.KEYCODE_E)||keys.contains(KeyEvent.KEYCODE_DPAD_RIGHT)?30:0)-(keys.contains(KeyEvent.KEYCODE_Q)||keys.contains(KeyEvent.KEYCODE_DPAD_LEFT)?30:0);
        targetPitch=(keys.contains(KeyEvent.KEYCODE_DPAD_DOWN)?30:0)-(keys.contains(KeyEvent.KEYCODE_DPAD_UP)?30:0);
    }
    /** SDL supplies normalized values from an actual connected controller. */
    public static synchronized void controllerTilt(int device,float horizontal,float vertical){
        if(!control.equals("controller")||!Float.isFinite(horizontal)||!Float.isFinite(vertical))return;
        targetRoll=Math.max(-1,Math.min(1,horizontal))*30;targetPitch=Math.max(-1,Math.min(1,vertical))*30;
    }
    private static synchronized float[] sample(long now){
        double seconds=updated==0?1.0/60:Math.max(0,(now-updated)*1e-9);
        if(seconds>=.0005){
            float dp=Math.max((float)(-180*seconds),Math.min((float)(180*seconds),targetPitch-pitch));
            float dr=Math.max((float)(-180*seconds),Math.min((float)(180*seconds),targetRoll-roll));
            pitch+=dp;roll+=dr;gyroPitch=(float)Math.toRadians(dp/seconds);gyroRoll=(float)Math.toRadians(dr/seconds);updated=now;
        }
        float x=(float)Math.toRadians(pitch),y=(float)Math.toRadians(roll);
        return new float[]{x,y,gyroPitch,gyroRoll};
    }
    private record Reading(int encoding,float[] values){}
    private static Reading reading(int type,float[] state){
        int encoding=1;float[] result=new float[16];double x=state[0],y=state[1];
        switch(type){
            case Sensor.TYPE_ACCELEROMETER,Sensor.TYPE_GRAVITY->{encoding=0;result[0]=(float)(-Math.sin(y)*SensorManager.GRAVITY_EARTH);result[1]=(float)(Math.sin(x)*Math.cos(y)*SensorManager.GRAVITY_EARTH);result[2]=(float)(Math.cos(x)*Math.cos(y)*SensorManager.GRAVITY_EARTH);}
            case Sensor.TYPE_GYROSCOPE->{encoding=0;result[0]=state[2];result[1]=state[3];}
            case Sensor.TYPE_ROTATION_VECTOR,Sensor.TYPE_GAME_ROTATION_VECTOR->{result[0]=(float)(Math.sin(x/2)*Math.cos(y/2));result[1]=(float)(Math.cos(x/2)*Math.sin(y/2));result[2]=(float)(-Math.sin(x/2)*Math.sin(y/2));result[3]=(float)(Math.cos(x/2)*Math.cos(y/2));result[4]=-1;}
            case Sensor.TYPE_ORIENTATION->{encoding=0;result[1]=(float)Math.toDegrees(x);result[2]=(float)Math.toDegrees(y);}
            case Sensor.TYPE_LINEAR_ACCELERATION->{encoding=0;} // Manual orientation has no translation.
            default->throw new IllegalArgumentException("Unknown manual motion sensor");
        }
        return new Reading(encoding,result);
    }
    /** A transport borrows the existing source; it owns no motion state or controls. */
    static synchronized List<Sensor> retainPublication(){
        var sensors=new ArrayList<Sensor>();
        for(int index=0;;index++){
            Sensor sensor=(Sensor)HostUiAccess.construct(Sensor.class,new Class<?>[0]);
            if(!nativeGetSensorAtIndex(1,sensor,index))break;sensors.add(sensor);
        }
        publications++;return List.copyOf(sensors);
    }
    static synchronized void releasePublication(){if(publications<=0)throw new IllegalStateException("Sensor source is already released");publications--;}
    static int publicationEncoding(int type){return reading(type,new float[4]).encoding();}
    static synchronized void copyPublication(long now,List<Sensor> sensors,java.nio.FloatBuffer output){
        float[] state=sample(now);for(Sensor sensor:sensors)output.put(reading(sensor.getType(),state).values());
    }
    private static boolean supported(int handle){for(int type:TYPES)if(type==handle)return true;return false;}
    public static void nativeClassInit(){}
    public static long nativeCreate(String packageName){Objects.requireNonNull(packageName);return 1;}
    public static synchronized boolean nativeGetSensorAtIndex(long instance,Sensor sensor,int index){
        if(instance!=1||control.equals("off")||index<0||index>=TYPES.length)return false;
        int type=TYPES[index];HostUiAccess.call(SET_TYPE,sensor,type);
        String label=switch(type){case Sensor.TYPE_ACCELEROMETER->"accelerometer";case Sensor.TYPE_GRAVITY->"gravity";case Sensor.TYPE_GYROSCOPE->"gyroscope";case Sensor.TYPE_LINEAR_ACCELERATION->"linear acceleration";case Sensor.TYPE_ROTATION_VECTOR->"rotation vector";case Sensor.TYPE_GAME_ROTATION_VECTOR->"game rotation vector";default->"orientation";};
        HostUiAccess.set(sensor,Sensor.class,"mName","Manual tilt "+label);HostUiAccess.set(sensor,Sensor.class,"mVendor","Android");
        HostUiAccess.set(sensor,Sensor.class,"mVersion",1);HostUiAccess.set(sensor,Sensor.class,"mHandle",type);HostUiAccess.set(sensor,Sensor.class,"mId",type);
        HostUiAccess.set(sensor,Sensor.class,"mUuid",new UUID(0,type));HostUiAccess.set(sensor,Sensor.class,"mRequiredPermission","");
        float range=switch(type){case Sensor.TYPE_ACCELEROMETER,Sensor.TYPE_GRAVITY,Sensor.TYPE_LINEAR_ACCELERATION->SensorManager.GRAVITY_EARTH;case Sensor.TYPE_GYROSCOPE->(float)Math.PI;case Sensor.TYPE_ORIENTATION->360;default->1;};
        HostUiAccess.set(sensor,Sensor.class,"mMaxRange",range);HostUiAccess.set(sensor,Sensor.class,"mResolution",.0001f);HostUiAccess.set(sensor,Sensor.class,"mPower",0f);
        HostUiAccess.set(sensor,Sensor.class,"mMinDelay",PERIOD_US);HostUiAccess.set(sensor,Sensor.class,"mMaxDelay",1_000_000);HostUiAccess.set(sensor,Sensor.class,"mFlags",0);return true;
    }
    public static boolean nativeGetDefaultDeviceSensorAtIndex(long instance,Sensor sensor,int index){return nativeGetSensorAtIndex(instance,sensor,index);}
    public static void nativeGetDynamicSensors(long instance,List<Sensor> sensors){} // Session controls are fixed before framework construction.
    public static void nativeGetRuntimeSensors(long instance,int device,List<Sensor> sensors){}
    public static boolean nativeIsDataInjectionEnabled(long instance){return false;}
    public static boolean nativeIsReplayDataInjectionEnabled(long instance){return false;}
    public static boolean nativeIsHalBypassReplayDataInjectionEnabled(long instance){return false;}
    public static int nativeCreateDirectChannel(long instance,int device,long size,int kind,int fd,HardwareBuffer buffer){return -38;}
    public static void nativeDestroyDirectChannel(long instance,int channel){}
    public static int nativeConfigDirectChannel(long instance,int channel,int sensor,int rate){return -22;}
    public static int nativeSetOperationParameter(long instance,int handle,int type,float[] floats,int[] integers){return -38;}
    private static final class Rate {final long period;long next;Rate(int micros){period=Math.max(PERIOD_US,micros)*1000L;}}
    private static final class Queue implements AutoCloseable,Runnable {
        final WeakReference<?> receiver;final Handler handler;final Map<Integer,Rate> active=new LinkedHashMap<>();final Runnable retirement=this::close;
        boolean closed,running,scheduled;
        Queue(WeakReference<?> receiver,Looper looper){this.receiver=receiver;handler=new Handler(looper);if(!HostLooper.addShutdownListener(looper,retirement))closed=true;}
        private boolean schedule(long delay){if(running||scheduled)return true;scheduled=handler.postDelayed(this,delay);return scheduled;}
        synchronized int enable(int handle,int micros){if(closed||!supported(handle)||micros<0)return -22;active.put(handle,new Rate(micros));if(scheduled){handler.removeCallbacks(this);scheduled=false;}if(schedule(0))return 0;active.remove(handle);return -19;}
        synchronized int disable(int handle){active.remove(handle);if(active.isEmpty()){handler.removeCallbacks(this);scheduled=false;}return 0;}
        @Override public void run(){
            Object target;List<Integer> ready=new ArrayList<>();long now=SystemClock.elapsedRealtimeNanos(),delay=Long.MAX_VALUE;
            synchronized(this){scheduled=false;if(closed)return;running=true;target=receiver.get();if(target==null){running=false;close();return;}for(var entry:active.entrySet()){Rate rate=entry.getValue();if(rate.next<=now){ready.add(entry.getKey());rate.next=now+rate.period;}}}
            try{float[] state=sample(now);for(int handle:ready){synchronized(this){if(closed||!active.containsKey(handle))continue;}HostUiAccess.call(DISPATCH,target,handle,reading(handle,state).values(),SensorManager.SENSOR_STATUS_ACCURACY_HIGH,now);}}
            finally{synchronized(this){running=false;if(!closed&&!active.isEmpty()){now=SystemClock.elapsedRealtimeNanos();for(Rate rate:active.values())delay=Math.min(delay,rate.next-now);schedule(Math.max(0,(delay+999_999)/1_000_000));}}}
        }
        synchronized int flush(){if(closed)return -19;int[] handles=active.keySet().stream().mapToInt(Integer::intValue).toArray();return handler.post(()->{Object target=receiver.get();if(target==null)return;for(int handle:handles){synchronized(this){if(closed||!active.containsKey(handle))continue;}HostUiAccess.call(FLUSH,target,handle);}})?0:-19;}
        @Override public synchronized void close(){if(closed)return;closed=true;scheduled=false;active.clear();handler.removeCallbacksAndMessages(null);HostLooper.removeShutdownListener(handler.getLooper(),retirement);synchronized(HostSensors.class){queues.remove(this);}}
    }
    public static long nativeInitBaseEventQueue(long instance,WeakReference<?> receiver,MessageQueue messages,String packageName,int mode,String opPackageName,String attribution){
        if(instance!=1||mode!=0)return 0;Looper looper=HostLooper.forQueue(messages);if(looper==null)throw new IllegalArgumentException("Sensor event queue has no Looper owner");
        Queue queue=new Queue(receiver,looper);if(queue.closed)return 0;synchronized(HostSensors.class){queues.add(queue);}try{return HostNativeHandle.create(queue);}catch(Throwable failure){queue.close();throw failure;}
    }
    private static Queue queue(long handle){return (Queue)HostNativeHandle.state(handle);}
    public static int nativeEnableSensor(long handle,int sensor,int rateUs,int latencyUs){return queue(handle).enable(sensor,rateUs);}
    public static int nativeDisableSensor(long handle,int sensor){return queue(handle).disable(sensor);}
    public static void nativeDestroySensorEventQueue(long handle){HostNativeHandle.release(handle);}
    public static int nativeFlushSensor(long handle){return queue(handle).flush();}
    public static int nativeInjectSensorData(long handle,int sensor,float[] values,int accuracy,long timestamp){return -1;}
    public static void shutdown(){List<Queue> active;synchronized(HostSensors.class){active=List.copyOf(queues);reset();}for(Queue queue:active)queue.close();}
}
