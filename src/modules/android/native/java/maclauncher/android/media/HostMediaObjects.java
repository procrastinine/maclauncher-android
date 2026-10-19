package maclauncher.android.media;

import java.lang.ref.Cleaner;
import java.lang.reflect.Field;
import java.util.*;
import android.os.Looper;
import maclauncher.android.framework.HostLooper;

/** Opaque Java-side peers for native Android media handles, with one lifetime owner. */
final class HostMediaObjects {
    interface State extends AutoCloseable {@Override void close();}
    private record Peer(State state,Cleaner.Cleanable cleaner) {}
    private static final Cleaner CLEANER=Cleaner.create();
    private static final Map<Long,Peer> peers=new HashMap<>();
    private static final Map<Class<?>,Field> fields=new HashMap<>();
    private static long next;
    private static boolean registered,closed;
    private static Field field(Object object) {
        Class<?> type=object instanceof android.media.AudioTrack?android.media.AudioTrack.class:object instanceof android.media.MediaPlayer?android.media.MediaPlayer.class:
            object instanceof android.media.AudioRecord?android.media.AudioRecord.class:android.hardware.Camera.class;
        return fields.computeIfAbsent(type,key->{try{Field value=key.getDeclaredField(key==android.media.AudioRecord.class?"mNativeAudioRecordHandle":key==android.media.AudioTrack.class?"mNativeTrackInJavaObj":"mNativeContext");value.setAccessible(true);return value;}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}});
    }
    private static long id(Object object){try{return field(object).getLong(object);}catch(IllegalAccessException error){throw new AssertionError(error);}}
    private static void id(Object object,long value){try{field(object).setLong(object,value);}catch(IllegalAccessException error){throw new AssertionError(error);}}
    static synchronized void attach(Object object,State state) {
        try {
            if(closed||next==Long.MAX_VALUE)throw new IllegalStateException("Media object owner is closed or identities are exhausted");
            if(id(object)!=0)throw new IllegalStateException("Media object already initialized");
            if(!registered&&Looper.getMainLooper()!=null){if(!HostLooper.addShutdownListener(Looper.getMainLooper(),HostMediaObjects::closeAll))throw new IllegalStateException("Media owner has stopped");registered=true;}
            long handle=++next;Cleaner.Cleanable cleanup=CLEANER.register(object,()->retire(handle));peers.put(handle,new Peer(state,cleanup));id(object,handle);
        } catch(RuntimeException|Error error){state.close();throw error;}
    }
    static synchronized <T extends State>T get(Object object,Class<T> type){Peer peer=peers.get(id(object));if(peer==null)throw new IllegalStateException("Media object is released or uninitialized");return type.cast(peer.state);}
    static void release(Object object){Peer peer;synchronized(HostMediaObjects.class){peer=peers.get(id(object));id(object,0);}if(peer!=null)peer.cleaner.clean();}
    private static void retire(long id){Peer peer;synchronized(HostMediaObjects.class){peer=peers.remove(id);}if(peer!=null)peer.state.close();}
    private static void closeAll(){List<Peer> snapshot;synchronized(HostMediaObjects.class){closed=true;snapshot=List.copyOf(peers.values());}Throwable first=null;for(Peer peer:snapshot)try{peer.cleaner.clean();}catch(Throwable error){if(first==null)first=error;else if(first!=error)first.addSuppressed(error);}if(first instanceof RuntimeException e)throw e;if(first instanceof Error e)throw e;}
}
