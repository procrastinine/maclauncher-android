package maclauncher.android.media;

import android.content.Context;
import android.content.Intent;
import android.media.*;
import android.os.*;
import java.lang.reflect.*;
import java.util.*;
import maclauncher.android.framework.HostLooper;
import maclauncher.android.framework.HostServices;
import maclauncher.android.framework.HostAudioPolicy;
import maclauncher.android.bridge.LocalBroadcasts;

/** Android's original focus/player policy and application-local Klepton output gain. */
public final class HostAudioService {
    private HostAudioService() {}
    private static final Map<Integer,Long> lastActive=new HashMap<>();
    private static boolean installed;
    private static volatile boolean closed;
    private static final Object policyLock=new Object();
    private static HostAudioPolicy policy;
    private static volatile boolean navigationRepeatSounds,homeSound;
    private static boolean soundAssetsReported;
    private static Handler handler;
    private static HostAudioManager output;
    private static Context context;
    private static volatile Runnable permissionBarrier=()->{};
    private static final IAudioManagerNative nativeService=new IAudioManagerNative.Stub(){
        @Override public void playbackHardeningEvent(int uid,byte type,boolean bypassed){
            if(Binder.getCallingUid()!=android.os.Process.AUDIOSERVER_UID)return;
            String counter=switch(type){
                case IAudioManagerNative.HardeningType.PARTIAL->"media_audio.value_audio_playback_hardening_partial_restriction";
                case IAudioManagerNative.HardeningType.FULL->"media_audio.value_audio_playback_hardening_strict_would_restrict";
                default->throw new IllegalArgumentException("Invalid audio hardening type");
            };
            com.android.modules.expresslog.Counter.logIncrementWithUid(counter,uid);
            android.util.Log.i("AudioService","Audio playback hardening uid="+uid+" type="+type+" bypassed="+bypassed);
        }
        @Override public void permissionUpdateBarrier(){permissionBarrier.run();}
        @Override public void portMuteEvent(int port,int event){policy().portMute(port,event);}
    };
    /** The VM bootstrap connects its existing native policy permission owner. */
    public static void nativePermissionBarrier(Runnable update){permissionBarrier=Objects.requireNonNull(update);}
    public static IBinder nativeEndpoint(){return maclauncher.android.framework.HostServiceEndpoints.export(service,"android.media.IAudioService",context);}
    /** Publish the system process's existing complete audio endpoint. */
    public static AutoCloseable publishNative(Runnable update)throws RemoteException{
        nativePermissionBarrier(update);IBinder endpoint=nativeEndpoint();
        HostServices.unregister(Context.AUDIO_SERVICE,service,"android.media.IAudioService");
        HostServices.manager().addService(Context.AUDIO_SERVICE,endpoint,false,0);
        return ()->{HostServices.manager().tryUnregisterService(Context.AUDIO_SERVICE,endpoint);permissionBarrier=()->{};};
    }
    private static final Binder binder=new Binder();
    private static final IAudioService service=(IAudioService)Proxy.newProxyInstance(IAudioService.class.getClassLoader(),new Class<?>[]{IAudioService.class},HostAudioService::invoke);
    static {binder.attachInterface(service,"android.media.IAudioService");}
    public static synchronized void install(){install(null);}
    public static synchronized void install(HostAudioManager owner){install(owner,null);}
    public static synchronized void install(HostAudioManager owner,Context application){
        if(closed)throw new IllegalStateException("Audio service is closed");
        if(output==null&&owner!=null)output=owner;
        if(context==null&&application!=null){context=application;publishRingerMode(output().getRingerMode());}
        if(installed)return;
        handler=new Handler(Objects.requireNonNull(Looper.getMainLooper(),"Audio owner needs a main Looper"));
        HostServices.register(Context.AUDIO_SERVICE,service,"android.media.IAudioService");
        if(!HostLooper.addShutdownListener(handler.getLooper(),HostAudioService::close))throw new IllegalStateException("Audio owner has stopped");
        installed=true;
    }
    private static synchronized HostAudioManager output(){if(output==null)output=new HostAudioManager();return output;}
    private static HostAudioPolicy policy(){synchronized(policyLock){
        if(closed)throw new IllegalStateException("Audio service is closed");
        if(policy==null){HostAudioManager mixer=output();policy=new HostAudioPolicy(context,mixer.getStreamMaxVolume(AudioManager.STREAM_ALARM),mixer::isStreamMute);}
        return policy;
    }}
    private static void publishRingerMode(int mode){
        if(context==null)return;
        for(String action:new String[]{AudioManager.RINGER_MODE_CHANGED_ACTION,AudioManager.INTERNAL_RINGER_MODE_CHANGED_ACTION}){
            Intent intent=new Intent(action).putExtra(AudioManager.EXTRA_RINGER_MODE,mode)
                .addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT|Intent.FLAG_RECEIVER_REPLACE_PENDING);
            LocalBroadcasts.send(context,null,intent,null,null,0,null,null,null,null,null,false,true,context.getUserId());
        }
    }
    private static synchronized void setRingerMode(int mode,String caller){
        if(caller==null||caller.isEmpty())throw new IllegalArgumentException("Ringer mode caller is empty");
        Vibrator vibrator=context==null?null:context.getSystemService(Vibrator.class);
        if(output().setRingerMode(mode,vibrator!=null&&vibrator.hasVibrator()))publishRingerMode(output().getRingerMode());
    }
    /** The host mixer follows the current Mac output; a connected idle device is not an active route. */
    private static boolean outputUses(int type){
        for(AudioDeviceInfo device:AudioManager.getDevicesStatic(AudioManager.GET_DEVICES_OUTPUTS))
            if(device.getType()==type)return true;
        return false;
    }
    /** Android UI effects are optional system media assets, separate from game audio. */
    private static synchronized boolean loadSoundEffects(){
        if(!soundAssetsReported){soundAssetsReported=true;System.err.println("[android-host] Android system sound assets could not be found; game audio remains available.");}
        return false;
    }
    private static int stream(AudioPlaybackConfiguration player){return AudioAttributes.toLegacyStreamType(player.getAudioAttributes());}
    public static boolean streamActive(int stream,int pastMs){
        HostAudioPolicy current;synchronized(policyLock){current=policy;}
        if(current!=null)for(var player:current.playback())if(player.isActive()&&stream(player)==stream)return true;
        synchronized(lastActive){
            Long stopped=lastActive.get(stream);if(pastMs>0&&stopped!=null&&SystemClock.uptimeMillis()-stopped<pastMs)return true;
        }
        return HostAudioManager.streamActive(stream,pastMs);
    }
    private static void recordActive(int id){for(var player:policy().playback())if(player.getPlayerInterfaceId()==id&&player.isActive())synchronized(lastActive){lastActive.put(stream(player),SystemClock.uptimeMillis());}}
    private static void close(){
        HostAudioPolicy retiring;synchronized(policyLock){closed=true;retiring=policy;policy=null;}
        try{if(retiring!=null)retiring.close();}finally{
            synchronized(lastActive){lastActive.clear();}
            handler.removeCallbacksAndMessages(null);output=null;context=null;HostAudioDevices.shutdown();
        }
    }
    private static Object invoke(Object proxy,Method method,Object[] arguments)throws Throwable{
        Object[] a=arguments==null?new Object[0]:arguments;String name=method.getName();
        if(method.getDeclaringClass()==Object.class)return switch(name){case "toString"->"Local Android audio service";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==a[0];default->throw new AssertionError(name);};
        switch(name){
            case "asBinder":return binder;
            case "getNativeInterface":return nativeService;
            case "permissionUpdateBarrier":permissionBarrier.run();return null;
            case "trackPlayer":return policy().track((PlayerBase.PlayerIdCard)a[0]);
            case "playerAttributes":recordActive((int)a[0]);policy().attributes((int)a[0],(AudioAttributes)a[1]);return null;
            case "playerSessionId":policy().session((int)a[0],(int)a[1]);return null;
            case "playerEvent":recordActive((int)a[0]);policy().event((int)a[0],(int)a[1],(int[])a[2]);return null;
            case "portEvent":policy().portEvent((int)a[0],(int)a[1],(PersistableBundle)a[2]);return null;
            case "releasePlayer":recordActive((int)a[0]);policy().release((int)a[0]);return null;
            case "playerHasOpPlayAudio":policy().playerHasOp((int)a[0],(boolean)a[1]);return null;
            case "getActivePlaybackConfigurations":return policy().playback();
            case "isMusicActive":return !(boolean)a[0]&&streamActive(AudioManager.STREAM_MUSIC,0);
            case "registerPlaybackCallback":policy().registerPlayback((IPlaybackConfigDispatcher)a[0]);return null;
            case "unregisterPlaybackCallback":policy().unregisterPlayback((IPlaybackConfigDispatcher)a[0]);return null;
            case "areNavigationRepeatSoundEffectsEnabled":return navigationRepeatSounds;
            case "setNavigationRepeatSoundEffectsEnabled":navigationRepeatSounds=(boolean)a[0];return null;
            case "isHomeSoundEffectEnabled":return homeSound;
            case "setHomeSoundEffectEnabled":homeSound=(boolean)a[0];return null;
            case "loadSoundEffects":return loadSoundEffects();
            case "playSoundEffect":case "playSoundEffectVolume":loadSoundEffects();return null;
            case "unloadSoundEffects":return null; // No system sound assets are loaded.
            case "getStreamVolume":return output().getStreamVolume((int)a[0]);
            case "getLastAudibleStreamVolume":return output().getLastAudibleStreamVolume((int)a[0]);
            case "getStreamMinVolume":return output().getStreamMinVolume((int)a[0]);
            case "getStreamMaxVolume":return output().getStreamMaxVolume((int)a[0]);
            case "isStreamMute":return output().isStreamMute((int)a[0]);
            case "getRingerModeExternal":case "getRingerModeInternal":return output().getRingerMode();
            case "getMode":return output().getMode();
            case "isValidRingerMode":return output().isValidRingerMode((int)a[0]);
            case "isStreamAffectedByRingerMode":return output().isStreamAffectedByRingerMode((int)a[0]);
            case "setRingerModeExternal":case "setRingerModeInternal":setRingerMode((int)a[0],(String)a[1]);return null;
            case "isVolumeFixed":return false;
            case "isSpeakerphoneOn":return outputUses(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
            case "isBluetoothScoOn":return outputUses(AudioDeviceInfo.TYPE_BLUETOOTH_SCO);
            case "isBluetoothA2dpOn":return outputUses(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP);
            case "setStreamVolume":case "setStreamVolumeWithAttribution":output().setStreamVolume((int)a[0],(int)a[1],(int)a[2]);return null;
            case "adjustStreamVolume":case "adjustStreamVolumeWithAttribution":output().adjustStreamVolume((int)a[0],(int)a[1],(int)a[2]);return null;
            case "adjustSuggestedStreamVolume":output().adjustSuggestedStreamVolume((int)a[0],(int)a[1],(int)a[2]);return null;
            case "requestAudioFocus":return policy().request((AudioAttributes)a[0],(int)a[1],(IBinder)a[2],(IAudioFocusDispatcher)a[3],(String)a[4],(String)a[5],(int)a[7],(int)a[9]);
            case "abandonAudioFocus":return policy().abandon((IAudioFocusDispatcher)a[0],(String)a[1],(AudioAttributes)a[2],(String)a[3]);
            case "unregisterAudioFocusClient":policy().unregister((String)a[0]);return null;
            case "getCurrentAudioFocus":return policy().currentFocus();
            default:throw new RemoteException("Android audio operation is unavailable: "+name);
        }
    }
}
