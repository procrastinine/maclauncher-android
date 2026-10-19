package maclauncher.android.framework;

import android.content.Context;
import android.hardware.input.*;
import android.os.*;
import android.view.*;
import java.util.*;

/** SDL input-device and pointer-capture boundary for Android's complete InputManager. */
public final class HostInputManager extends IInputManager.Default {
    private static final HostInputManager SERVICE=new HostInputManager();
    private static final Map<ViewRootImpl,Capture> roots=new WeakHashMap<>();
    private record Device(InputDevice input,Set<Integer> keys){}
    private static final Map<Integer,Device> devices=new LinkedHashMap<>();
    private static final Map<IBinder,DeviceListener> listeners=new LinkedHashMap<>();
    private static int nextDeviceId=1;
    private static final class DeviceListener implements IBinder.DeathRecipient {
        final IInputDevicesChangedListener target;final IBinder binder;
        DeviceListener(IInputDevicesChangedListener target){this.target=target;binder=target.asBinder();}
        @Override public void binderDied(){synchronized(HostInputManager.class){listeners.remove(binder,this);}}
    }
    private static boolean installed;
    private static final class Capture {final IBinder token;boolean requested;Capture(IBinder token){this.token=token;}}
    private HostInputManager(){}
    public static synchronized void install(){if(installed)return;HostServices.register(Context.INPUT_SERVICE,SERVICE,"android.hardware.input.IInputManager");installed=true;}
    /** The input adapter supplies a device with its actual publisher-native key map. */
    public static void installKeyboard(InputDevice device){
        var keys=new HashSet<Integer>();for(int code=0;code<=KeyEvent.getMaxKeyCode();code++)if(HostInput.keyboardHasKey(code))keys.add(code);
        installDevice(Objects.requireNonNull(device),keys);
    }
    static synchronized int allocateDeviceId(){if(nextDeviceId==Integer.MAX_VALUE)throw new IllegalStateException("Input device identifiers are exhausted");return nextDeviceId++;}
    static void installDevice(InputDevice device,Set<Integer> keys){synchronized(HostInputManager.class){devices.put(device.getId(),new Device(device,Set.copyOf(keys)));}devicesChanged();}
    static void installGamepad(InputDevice device,Set<Integer> keys){installDevice(device,keys);}
    static void removeGamepad(int id){synchronized(HostInputManager.class){devices.remove(id);}devicesChanged();}
    private static int[] deviceIds(){return devices.keySet().stream().mapToInt(Integer::intValue).toArray();}
    private static void devicesChanged(){
        List<DeviceListener> targets;int[] snapshot;
        synchronized(HostInputManager.class){int[] ids=deviceIds();snapshot=new int[ids.length*2];for(int i=0;i<ids.length;i++){InputDevice device=devices.get(ids[i]).input();snapshot[2*i]=ids[i];snapshot[2*i+1]=device.getGeneration();}targets=List.copyOf(listeners.values());}
        for(DeviceListener listener:targets)try{listener.target.onInputDevicesChanged(snapshot.clone());}catch(RemoteException failure){listener.binderDied();listener.binder.unlinkToDeath(listener,0);}
    }
    @Override public void registerInputDevicesChangedListener(IInputDevicesChangedListener listener)throws RemoteException{
        Objects.requireNonNull(listener);synchronized(HostInputManager.class){IBinder binder=Objects.requireNonNull(listener.asBinder());if(listeners.containsKey(binder))return;DeviceListener owner=new DeviceListener(listener);binder.linkToDeath(owner,0);listeners.put(binder,owner);}
    }
    static synchronized void attach(ViewRootImpl root,IBinder nativeWindow){roots.put(root,new Capture(Objects.requireNonNull(nativeWindow)));}
    static synchronized void detach(ViewRootImpl root){roots.remove(root);}
    /** Only used if the raw ViewRoot has no Android InputChannel. */
    public static synchronized IBinder inputToken(ViewRootImpl root,IBinder androidToken){if(androidToken!=null)return androidToken;Capture state=roots.get(root);return state==null?null:state.token;}
    @Override public InputDevice getInputDevice(int id){synchronized(HostInputManager.class){Device device=devices.get(id);return device==null?null:device.input();}}
    @Override public int[] getInputDeviceIds(){synchronized(HostInputManager.class){return deviceIds();}}
    @Override public KeyCharacterMap getKeyCharacterMap(String descriptor){synchronized(HostInputManager.class){for(Device device:devices.values())if(Objects.equals(device.input().getDescriptor(),descriptor))return device.input().getKeyCharacterMap();return null;}}
    @Override public boolean hasKeys(int deviceId,int sourceMask,int[] codes,boolean[] exists){
        if(codes==null||exists==null||exists.length<codes.length)throw new IllegalArgumentException("Invalid input key query");Arrays.fill(exists,false);
        synchronized(HostInputManager.class){
            for(Device state:devices.values()){InputDevice device=state.input();if(deviceId!=-1&&deviceId!=device.getId())continue;if(sourceMask!=0&&(device.getSources()&sourceMask&~InputDevice.SOURCE_CLASS_MASK)==0)continue;for(int i=0;i<codes.length;i++)exists[i]|=state.keys().contains(codes[i]);}
        }
        return true;
    }
    @Override public String getVelocityTrackerStrategy(){return "";}
    @Override public int isInTabletMode(){return InputManager.SWITCH_STATE_UNKNOWN;}
    @Override public int isMicMuted(){return InputManager.SWITCH_STATE_UNKNOWN;}
    @Override public KeyboardLayout[] getKeyboardLayouts(){return new KeyboardLayout[0];}
    @Override public Map getModifierKeyRemapping(){return Map.of();}
    @Override public int[] getVibratorIds(int device){return new int[0];}
    @Override public InputSensorInfo[] getSensorList(int device){return new InputSensorInfo[0];}
    @Override public List<android.hardware.lights.Light> getLights(int device){return List.of();}
    @Override public void requestPointerCapture(IBinder token,boolean enabled){synchronized(HostInputManager.class){for(Capture state:roots.values())if(state.token==token){state.requested=enabled;return;}}throw new IllegalArgumentException("Pointer capture requires an attached native window");}
    public static boolean captureRequested(View view){synchronized(HostInputManager.class){Capture state=roots.get(view.getViewRootImpl());return state!=null&&state.requested;}}
    /** An actual SDL relative-mouse-mode acknowledgement, not a fabricated state update. */
    public static void captureChanged(View view,boolean captured){ViewRootImpl root=view.getViewRootImpl();if(root==null)return;synchronized(HostInputManager.class){Capture state=roots.get(root);if(state==null)return;if(!captured)state.requested=false;}HostUiAccess.call(HostUiAccess.method(ViewRootImpl.class,"handlePointerCaptureChanged",boolean.class),root,captured);}
    @Override public boolean injectInputEvent(InputEvent event,int mode){throw new SecurityException("System input injection is unavailable");}
    @Override public boolean injectInputEventToTarget(InputEvent event,int mode,int uid){throw new SecurityException("System input injection is unavailable");}
    @Override public void enableInputDevice(int device){throw new SecurityException("Native input-device configuration is unavailable");}
    @Override public void disableInputDevice(int device){throw new SecurityException("Native input-device configuration is unavailable");}
    public static void shutdown(){List<DeviceListener> retired;synchronized(HostInputManager.class){roots.clear();devices.clear();retired=List.copyOf(listeners.values());listeners.clear();}for(DeviceListener listener:retired)listener.binder.unlinkToDeath(listener,0);}
}
