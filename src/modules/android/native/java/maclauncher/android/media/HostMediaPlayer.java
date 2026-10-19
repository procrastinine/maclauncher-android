package maclauncher.android.media;

import android.media.*;
import android.os.*;
import java.io.*;
import java.lang.reflect.*;
import java.util.concurrent.*;

/** Native media-player boundary. AOSP retains listeners, attributes and public overloads. */
public final class HostMediaPlayer {
    private HostMediaPlayer() {}
    private static final Method EVENT;
    static {try{EVENT=MediaPlayer.class.getDeclaredMethod("postEventFromNative",Object.class,int.class,int.class,int.class,Object.class);EVENT.setAccessible(true);}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    private static final ThreadPoolExecutor prepareWorker=new ThreadPoolExecutor(0,1,10,TimeUnit.SECONDS,new LinkedBlockingQueue<>(),task->{Thread thread=new Thread(task,"Android media preparation");thread.setDaemon(true);return thread;});
    private static boolean preparationAttached;
    private static synchronized void attachPreparation(){
        if(preparationAttached)return;
        if(!maclauncher.android.framework.HostLooper.addShutdownListener(Looper.getMainLooper(),()->{
            for(Runnable task:prepareWorker.shutdownNow())if(task instanceof Future<?> future)future.cancel(false);
        }))throw new IllegalStateException("Media preparation owner has stopped");
        preparationAttached=true;
    }
    public static void native_init(){AudioHost.initialize();}
    private static HostPlayer player(MediaPlayer owner){return HostMediaObjects.get(owner,HostPlayer.class);}
    private static void event(Object reference,int type,int first,int second){try{EVENT.invoke(null,reference,type,first,second,null);}catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}}
    public static void native_setup(MediaPlayer owner,Object reference,Parcel attribution,int session){HostAudioService.install();attachPreparation();HostPlayer peer=new HostPlayer();peer.session(session);peer.setOnCompletionListener(ignored->event(reference,2,0,0));peer.onChainedStart(()->event(reference,200,2,0));HostMediaObjects.attach(owner,peer);}
    public static void native_finalize(MediaPlayer owner){HostMediaObjects.release(owner);}
    public static void _release(MediaPlayer owner){HostMediaObjects.release(owner);}
    public static void _reset(MediaPlayer owner){player(owner).reset();}
    public static void nativeSetDataSource(MediaPlayer owner,IBinder http,String path,String[] keys,String[] values)throws IOException{if(http!=null||path.startsWith("http:")||path.startsWith("https:"))throw new IOException("Network media sources are unavailable");player(owner).setDataSource(path);}
    public static void _setDataSource(MediaPlayer owner,FileDescriptor fd,long offset,long length)throws IOException{player(owner).setDataSource(fd,offset,length);}
    public static int _prepare(MediaPlayer owner,Parcel playerId)throws IOException{player(owner).prepare();return 0;}
    public static int _prepareAsync(MediaPlayer owner,Parcel playerId){Object reference=new java.lang.ref.WeakReference<>(owner);player(owner).prepareAsync(prepareWorker,()->event(reference,1,0,0),()->event(reference,100,1,-1010));return 0;}
    public static void _start(MediaPlayer owner){player(owner).start();}
    public static void _stop(MediaPlayer owner){player(owner).stop();}
    public static void _pause(MediaPlayer owner){player(owner).pause();}
    public static void _seekTo(MediaPlayer owner,long millis,int mode){player(owner).seek(millis);event(new java.lang.ref.WeakReference<>(owner),4,0,0);}
    public static int getCurrentPosition(MediaPlayer owner){return player(owner).position();}
    public static int getDuration(MediaPlayer owner){return player(owner).duration();}
    public static boolean isPlaying(MediaPlayer owner){return player(owner).isPlaying();}
    public static boolean isLooping(MediaPlayer owner){return player(owner).isLooping();}
    public static void setLooping(MediaPlayer owner,boolean value){player(owner).setLooping(value);}
    public static void setNextMediaPlayer(MediaPlayer owner,MediaPlayer next){if(owner==next)throw new IllegalArgumentException("A MediaPlayer cannot follow itself");player(owner).next(next==null?null:player(next));}
    public static void _setVolume(MediaPlayer owner,float left,float right){player(owner).setVolume(left,right);}
    public static void _setAudioStreamType(MediaPlayer owner,int stream){player(owner).setAudioStreamType(stream);}
    public static int _getAudioStreamType(MediaPlayer owner){return player(owner).streamType();}
    public static void native_setAudioSessionId(MediaPlayer owner,int session){player(owner).session(session);}
    public static int getAudioSessionId(MediaPlayer owner){return player(owner).session();}
    public static int getVideoWidth(MediaPlayer owner){player(owner);return 0;}
    public static int getVideoHeight(MediaPlayer owner){player(owner);return 0;}
    public static boolean setParameter(MediaPlayer owner,int key,Parcel value){HostPlayer peer=player(owner);if(key!=1400)return false;int position=value.dataPosition();try{value.setDataPosition(0);peer.setAudioAttributes(AudioAttributes.CREATOR.createFromParcel(value));return true;}finally{value.setDataPosition(position);}}
    public static int native_invoke(MediaPlayer owner,Parcel request,Parcel reply){HostPlayer peer=player(owner);int position=request.dataPosition();try{request.setDataPosition(0);request.enforceInterface("android.media.IMediaPlayer");int method=request.readInt();switch(method){
        case 1->{peer.duration();reply.writeInt(1);reply.writeInt(1);reply.writeInt(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);reply.writeString("audio/unknown");reply.writeString("und");reply.writeBoolean(false);return 0;}
        case 7->{reply.writeInt(request.readInt()==MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO?0:-1);return 0;}
        default->{return -38;}
    }}finally{request.setDataPosition(position);}}
    public static boolean native_getMetadata(MediaPlayer owner,boolean update,boolean filter,Parcel reply){player(owner);return false;}
    public static int native_setMetadataFilter(MediaPlayer owner,Parcel request){player(owner);return -38;}
    public static PersistableBundle native_getMetrics(MediaPlayer owner){HostPlayer peer=player(owner);PersistableBundle result=new PersistableBundle();result.putString("android.media.mediaplayer.mediatype","audio");return result;}
    public static boolean native_setOutputDevice(MediaPlayer owner,int device){player(owner);return HostAudioRoute.select(device);}
    public static int[] native_getRoutedDeviceIds(MediaPlayer owner){player(owner);return HostAudioRoute.devices();}
    public static void native_enableDeviceCallback(MediaPlayer owner,boolean enabled){Object reference=new java.lang.ref.WeakReference<>(owner);player(owner).routing(()->event(reference,10000,0,0),enabled);}
}
