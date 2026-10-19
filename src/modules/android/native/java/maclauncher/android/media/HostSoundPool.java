package maclauncher.android.media;

import android.media.*;
import android.os.Looper;
import java.io.*;
import java.lang.ref.Cleaner;
import java.lang.reflect.*;
import java.util.*;
import maclauncher.android.framework.HostLooper;

/** Native-side SoundPool ownership. Public APIs and callbacks remain in AOSP. */
public final class HostSoundPool {
    private HostSoundPool() {}
    private static final Cleaner cleaner=Cleaner.create();
    private static final Map<Long,Peer> peers=new HashMap<>();
    private static long nextId;
    private static boolean shutdownRegistered,closed;
    private static final Field context;
    private static final Method event;
    static {
        try {context=SoundPool.class.getDeclaredField("mNativeContext");context.setAccessible(true);event=SoundPool.class.getDeclaredMethod("postEventFromNative",int.class,int.class,int.class,Object.class);event.setAccessible(true);}
        catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}
    }
    private record Peer(Engine engine,Cleaner.Cleanable cleanup) {}
    private static long id(SoundPool owner){try{return context.getLong(owner);}catch(IllegalAccessException error){throw new AssertionError(error);}}
    private static void id(SoundPool owner,long id){try{context.setLong(owner,id);}catch(IllegalAccessException error){throw new AssertionError(error);}}
    private static synchronized Engine peer(SoundPool owner){Peer peer=peers.get(id(owner));return peer==null?null:peer.engine;}
    private static synchronized void retire(long id){Peer peer=peers.remove(id);if(peer!=null)peer.engine.release();}
    private static void closeAll(){List<Peer> owned;synchronized(HostSoundPool.class){closed=true;owned=List.copyOf(peers.values());}for(Peer peer:owned)peer.cleanup.clean();}
    public static synchronized int native_setup(SoundPool owner,int count,Object attributes,String packageName){
        if(closed||peers.size()>=64||nextId==Long.MAX_VALUE)return -1;
        if(id(owner)!=0)throw new IllegalStateException("Sound pool already initialized");
        if(!shutdownRegistered&&Looper.getMainLooper()!=null){if(!HostLooper.addShutdownListener(Looper.getMainLooper(),HostSoundPool::closeAll))return -1;shutdownRegistered=true;}
        HostAudioService.install();
        var engine=new Engine(count,AudioAttributes.toLegacyStreamType((AudioAttributes)Objects.requireNonNull(attributes)),0);
        long id=++nextId;
        try {var cleanup=cleaner.register(owner,()->retire(id));peers.put(id,new Peer(engine,cleanup));id(owner,id);return 0;}
        catch(RuntimeException|Error error){engine.release();throw error;}
    }
    public static void native_release(SoundPool owner){Peer peer;synchronized(HostSoundPool.class){peer=peers.get(id(owner));id(owner,0);}if(peer!=null)peer.cleanup.clean();}
    public static int _load(SoundPool owner,FileDescriptor fd,long offset,long length,int priority){
        Engine engine=peer(owner);if(engine==null)return 0;
        int sample=engine.load(fd,offset,length,priority);
        if(sample!=0)try{event.invoke(owner,1,sample,0,null);}catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}
        return sample;
    }
    public static int _play(SoundPool owner,int sample,float left,float right,int priority,int loop,float rate,int playerId){Engine e=peer(owner);return e==null?0:e.play(sample,left,right,priority,loop,rate);}
    public static boolean unload(SoundPool owner,int sample){Engine e=peer(owner);return e!=null&&e.unload(sample);}
    public static void pause(SoundPool owner,int stream){Engine e=peer(owner);if(e!=null)e.pause(stream);}
    public static void resume(SoundPool owner,int stream){Engine e=peer(owner);if(e!=null)e.resume(stream);}
    public static void stop(SoundPool owner,int stream){Engine e=peer(owner);if(e!=null)e.stop(stream);}
    public static void autoPause(SoundPool owner){Engine e=peer(owner);if(e!=null)e.autoPause();}
    public static void autoResume(SoundPool owner){Engine e=peer(owner);if(e!=null)e.autoResume();}
    public static void setPriority(SoundPool owner,int stream,int priority){Engine e=peer(owner);if(e!=null)e.setPriority(stream,priority);}
    public static void setLoop(SoundPool owner,int stream,int loop){Engine e=peer(owner);if(e!=null)e.setLoop(stream,loop);}
    public static void setRate(SoundPool owner,int stream,float rate){Engine e=peer(owner);if(e!=null)e.setRate(stream,rate);}
    public static void _setVolume(SoundPool owner,int stream,float left,float right){Engine e=peer(owner);if(e!=null)e.setVolume(stream,left,right);}
    public static void _mute(SoundPool owner,boolean value){Engine e=peer(owner);if(e!=null)e.mute(value);}

    private static final class Engine {
    private static final class Stream {
        final long peer;int priority,loop;boolean paused,autoPaused;float left,right,rate;
        Stream(long peer,int priority,float left,float right,int loop,float rate){this.peer=peer;this.priority=priority;this.left=left;this.right=right;this.loop=loop;this.rate=rate;}
    }
    private final int limit;private int streamType=AudioManager.STREAM_MUSIC;
    private final AudioHost.Ownership handles=new AudioHost.Ownership();
    private final java.lang.ref.Cleaner.Cleanable cleanup=AudioHost.track(this,handles);
    private final Map<Integer,Long> samples=new HashMap<>();
    private final LinkedHashMap<Integer,Stream> streams=new LinkedHashMap<>();
    private int nextSample=1,nextStream=1;private boolean released,muted;
    private Engine(int limit){if(limit<1||limit>256)throw new IllegalArgumentException("Invalid audio stream limit");this.limit=limit;}
    // macOS has one application audio route; legacy stream categories use that
    // same route. Android ignores srcQuality, so it does not select a decoder.
    public Engine(int maxStreams,int streamType,int srcQuality){this(maxStreams);this.streamType=maclauncher.android.media.HostAudioManager.checkedStream(streamType);}
    private boolean action(long peer,int action){try{return AudioHost.action(peer,action);}finally{java.lang.ref.Reference.reachabilityFence(this);}}
    // A released Android pool has no native SoundPool: load/play return zero,
    // unload returns false, and pause/resume do nothing. This differs from
    // MediaPlayer's state machine and keeps late sample cleanup harmless.
    public synchronized int load(FileDescriptor fd,long offset,long length,int priority){if(released)return 0;if(samples.size()>=4096)throw new IllegalStateException("Sound pool sample limit reached");if(offset<0||length<0)throw new IllegalArgumentException("Invalid audio descriptor range");try{long peer=handles.add(AudioHost.load(null,Objects.requireNonNull(fd),offset,length));int id=nextSample++;samples.put(id,peer);return id;}catch(IOException e){System.err.println("[android-audio] Cannot decode audio descriptor: "+e.getMessage());return 0;}}
    private void prune(){var it=streams.entrySet().iterator();while(it.hasNext()){Stream stream=it.next().getValue();if(!stream.paused&&!action(stream.peer,4)){handles.release(stream.peer);it.remove();}}}
    public synchronized int play(int sample,float left,float right,int priority,int loop,float rate){if(released)return 0;Long template=samples.get(sample);if(template==null)return 0;prune();
        if(streams.size()>=limit){var victim=streams.entrySet().stream().min(Comparator.comparingInt(e->e.getValue().priority)).orElseThrow();if(victim.getValue().priority>priority)return 0;handles.release(victim.getValue().peer);streams.remove(victim.getKey());}
        long peer=handles.add(AudioHost.copy(template));try{AudioHost.setStream(peer,streamType);AudioHost.configure(peer,muted?0:left,muted?0:right,loop,rate);if(!action(peer,0)){handles.release(peer);return 0;}}catch(RuntimeException e){handles.release(peer);throw e;}
        int id=nextStream++;streams.put(id,new Stream(peer,priority,left,right,loop,rate));return id;
    }
    public synchronized void pause(int id){if(released)return;Stream stream=streams.get(id);if(stream!=null){action(stream.peer,1);stream.paused=true;stream.autoPaused=false;}}
    public synchronized void resume(int id){if(released)return;Stream stream=streams.get(id);if(stream!=null&&stream.paused&&action(stream.peer,0)){stream.paused=false;stream.autoPaused=false;}}
    public synchronized void stop(int id){if(released)return;Stream stream=streams.remove(id);if(stream!=null)handles.release(stream.peer);}
    public synchronized void setPriority(int id,int priority){if(released)return;Stream stream=streams.get(id);if(stream!=null)stream.priority=priority;}
    public synchronized void setVolume(int id,float left,float right){if(released)return;Stream stream=streams.get(id);if(stream!=null){AudioHost.configure(stream.peer,muted?0:left,muted?0:right,stream.loop,stream.rate);stream.left=left;stream.right=right;}}
    public synchronized void setLoop(int id,int loop){if(released)return;Stream stream=streams.get(id);if(stream!=null){AudioHost.configure(stream.peer,muted?0:stream.left,muted?0:stream.right,loop,stream.rate);stream.loop=loop;}}
    public synchronized void setRate(int id,float rate){if(released)return;Stream stream=streams.get(id);if(stream!=null){AudioHost.configure(stream.peer,muted?0:stream.left,muted?0:stream.right,stream.loop,rate);stream.rate=rate;}}
    public synchronized void autoPause(){if(released)return;prune();for(Stream stream:streams.values()){if(!stream.paused&&action(stream.peer,4)){action(stream.peer,1);stream.paused=true;stream.autoPaused=true;}}}
    public synchronized void autoResume(){if(released)return;for(Stream stream:streams.values())if(stream.autoPaused){if(!action(stream.peer,0))throw new IllegalStateException("Audio output could not resume");stream.paused=false;stream.autoPaused=false;}}
    public synchronized boolean unload(int id){if(released)return false;Long peer=samples.remove(id);if(peer==null)return false;handles.release(peer);return true;}
    public synchronized void release(){if(released)return;cleanup.clean();streams.clear();samples.clear();released=true;}
    public synchronized void mute(boolean value){muted=value;for(Stream stream:streams.values())AudioHost.configure(stream.peer,value?0:stream.left,value?0:stream.right,stream.loop,stream.rate);}

    }
}
