package maclauncher.android.framework;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.*;
import android.os.*;
import com.android.server.audio.MediaFocusControl;
import com.android.server.audio.PlaybackActivityMonitor;
import com.android.server.audio.PlayerFocusEnforcer;
import java.lang.reflect.Method;
import java.util.*;
import java.util.function.Consumer;
import java.util.function.Function;

/** Lifetime and service entry points for Android's original focus and player owners. */
public final class HostAudioPolicy implements AutoCloseable {
    private static final Class<?> FOCUS=MediaFocusControl.class,PLAYERS=PlaybackActivityMonitor.class;
    private static final Method REQUEST=HostUiAccess.method(FOCUS,"requestAudioFocus",AudioAttributes.class,int.class,IBinder.class,IAudioFocusDispatcher.class,String.class,String.class,int.class,int.class,boolean.class,int.class,boolean.class);
    private static final Method ABANDON=HostUiAccess.method(FOCUS,"abandonAudioFocus",IAudioFocusDispatcher.class,String.class,AudioAttributes.class,String.class);
    private static final Method UNREGISTER=HostUiAccess.method(FOCUS,"unregisterAudioFocusClient",String.class);
    private static final Method CURRENT=HostUiAccess.method(FOCUS,"getCurrentAudioFocus");
    private static final Method STACK=HostUiAccess.method(FOCUS,"getFocusStack");
    private static final Method REGISTER_PLAYBACK=HostUiAccess.method(PLAYERS,"registerPlaybackCallback",IPlaybackConfigDispatcher.class,boolean.class);
    private static final Method UNREGISTER_PLAYBACK=HostUiAccess.method(PLAYERS,"unregisterPlaybackCallback",IPlaybackConfigDispatcher.class);
    private static final Method PLAYBACK=HostUiAccess.method(PLAYERS,"getActivePlaybackConfigurations",boolean.class);
    private final Context context;
    private final MediaFocusControl focus;
    private final PlaybackActivityMonitor players;
    private boolean closed;

    public HostAudioPolicy(Context context,int maxAlarmVolume,Function<Integer,Boolean> streamMuted){
        this.context=Objects.requireNonNull(context);
        players=(PlaybackActivityMonitor)HostUiAccess.construct(PLAYERS,
            new Class<?>[]{Context.class,int.class,Consumer.class,Function.class},context,maxAlarmVolume,
            (Consumer<AudioDeviceAttributes>)device->{},streamMuted);
        MediaFocusControl created;
        try{created=(MediaFocusControl)HostUiAccess.construct(FOCUS,new Class<?>[]{Context.class,PlayerFocusEnforcer.class},context,players);}
        catch(Throwable failure){stopWorker(players,PLAYERS,"mEventHandler");throw failure;}
        focus=created;
    }
    public int track(PlayerBase.PlayerIdCard card){return players.trackPlayer(card);}
    public void attributes(int id,AudioAttributes attributes){players.playerAttributes(id,attributes,Binder.getCallingUid());}
    public void session(int id,int session){players.playerSessionId(id,session,Binder.getCallingUid());}
    public void event(int id,int event,int[] devices){players.playerEvent(id,event,devices,Binder.getCallingUid());}
    public void portMute(int port,int event){players.portMuteEvent(port,event,Binder.getCallingUid());}
    public void portEvent(int port,int event,PersistableBundle extras){players.portEvent(port,event,extras,Binder.getCallingUid());}
    public void release(int id){players.releasePlayer(id,Binder.getCallingUid());}
    public void playerHasOp(int id,boolean allowed){players.playerHasOpPlayAudio(id,allowed,Binder.getCallingUid());}
    public void registerPlayback(IPlaybackConfigDispatcher callback){HostUiAccess.call(REGISTER_PLAYBACK,players,callback,false);}
    public void unregisterPlayback(IPlaybackConfigDispatcher callback){HostUiAccess.call(UNREGISTER_PLAYBACK,players,callback);}
    @SuppressWarnings("unchecked") public List<AudioPlaybackConfiguration> playback(){return (List<AudioPlaybackConfiguration>)HostUiAccess.call(PLAYBACK,players,true);}
    public int currentFocus(){return (int)HostUiAccess.call(CURRENT,focus);}
    public int request(AudioAttributes attributes,int gain,IBinder owner,IAudioFocusDispatcher dispatcher,String client,String packageName,int flags,int sdk){
        if((flags&AudioManager.AUDIOFOCUS_FLAG_TEST)!=0)throw new IllegalArgumentException("Invalid test flag");
        if(attributes==null||owner==null||client==null||packageName==null)return AudioManager.AUDIOFOCUS_REQUEST_FAILED;
        // A session has no registered external AudioPolicy. Phone focus locking
        // still requires Android's existing permission decision.
        if((flags&AudioManager.AUDIOFOCUS_FLAG_LOCK)!=0 &&
            (!AudioSystem.IN_VOICE_COMM_FOCUS_ID.equals(client)||context.checkCallingOrSelfPermission(android.Manifest.permission.MODIFY_PHONE_STATE)!=PackageManager.PERMISSION_GRANTED))
            return AudioManager.AUDIOFOCUS_REQUEST_FAILED;
        context.getSystemService(android.app.AppOpsManager.class).checkPackage(Binder.getCallingUid(),packageName);
        return (int)HostUiAccess.call(REQUEST,focus,attributes,gain,owner,dispatcher,client,packageName,flags,sdk,false,-1,false);
    }
    public int abandon(IAudioFocusDispatcher dispatcher,String client,AudioAttributes attributes,String packageName){return (int)HostUiAccess.call(ABANDON,focus,dispatcher,client,attributes,packageName);}
    public void unregister(String client){HostUiAccess.call(UNREGISTER,focus,client);}
    private static void stopWorker(Object owner,Class<?> type,String name){
        Handler handler=(Handler)HostUiAccess.get(owner,type,name);
        if(handler!=null){handler.removeCallbacksAndMessages(null);handler.getLooper().quit();}
    }
    @Override @SuppressWarnings("unchecked") public synchronized void close(){
        if(closed)return;closed=true;
        try{
            for(AudioFocusInfo entry:(List<AudioFocusInfo>)HostUiAccess.call(STACK,focus))unregister(entry.getClientId());
            for(AudioPlaybackConfiguration player:playback())players.releasePlayer(player.getPlayerInterfaceId(),player.getClientUid());
            Class<?> clientType=HostUiAccess.type("com.android.server.audio.PlaybackActivityMonitor$PlayMonitorClient");
            for(Object client:List.copyOf((Collection<?>)HostUiAccess.get(players,PLAYERS,"mClients")))
                unregisterPlayback((IPlaybackConfigDispatcher)HostUiAccess.get(client,clientType,"mDispatcherCb"));
            if(HostUiAccess.get(null,clientType,"sListenerDeathMonitor")==players)HostUiAccess.set(null,clientType,"sListenerDeathMonitor",null);
            if(AudioPlaybackConfiguration.sPlayerDeathMonitor==players)AudioPlaybackConfiguration.sPlayerDeathMonitor=null;
        }finally{stopWorker(focus,FOCUS,"mFocusHandler");stopWorker(players,PLAYERS,"mEventHandler");}
    }
}
