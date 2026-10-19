package maclauncher.android.media;

import android.media.*;
import android.os.*;
import java.lang.reflect.*;
import java.nio.*;
import java.util.*;

/** PCM capture native boundary using the existing selected-input owner and resampler. */
public final class HostAudioRecord {
    private HostAudioRecord() {}
    private static final Method EVENT;
    static {try{EVENT=AudioRecord.class.getDeclaredMethod("postEventFromNative",Object.class,int.class,int.class,int.class,Object.class);EVENT.setAccessible(true);}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    private static final class Capture implements HostMediaObjects.State {
        long token,frames,nextPeriod;final int channels,bufferBytes;final Object reference;
        boolean running;int marker,period;boolean marked;
        Capture(long token,int channels,int bufferBytes,Object reference){this.token=token;this.channels=channels;this.bufferBytes=bufferBytes;this.reference=reference;}
        public synchronized void close(){running=false;if(token!=0){MediaInputs.audioClose(token);token=0;}}
        synchronized int read(short[] target,int offset,int count){if(token==0||!running)return AudioRecord.ERROR_INVALID_OPERATION;int got=MediaInputs.audioRead(token,target,offset,count-count%channels);if(got>0){frames+=got/channels;if(marker>0&&!marked&&frames>=marker){marked=true;event(reference,2);}if(period>0&&frames>=nextPeriod){nextPeriod=frames+period;event(reference,3);}}return got;}
    }
    private static Capture capture(AudioRecord owner){return HostMediaObjects.get(owner,Capture.class);}
    private static void event(Object ref,int type){try{EVENT.invoke(null,ref,type,0,0,null);}catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}}
    public static int native_setup(AudioRecord owner,Object reference,Object attributes,int[] sampleRate,int channelMask,int channelIndexMask,int format,int bytes,int[] session,Parcel attribution,long existing,int history,int flags){
        if(!MediaInputs.isGranted("android.permission.RECORD_AUDIO"))return -1;
        if(existing!=0||history!=0)return AudioRecord.ERROR_INVALID_OPERATION;
        if(!(attributes instanceof AudioAttributes audio))return AudioRecord.ERROR_BAD_VALUE;
        // Telephony and remote-submix sources are different devices, not microphone aliases.
        int source=audio.getCapturePreset();
        if(source!=0&&source!=1&&source!=5&&source!=6&&source!=7&&source!=9&&source!=10)return AudioRecord.ERROR_INVALID_OPERATION;
        if(sampleRate==null||sampleRate.length==0||session==null||session.length==0)return AudioRecord.ERROR_BAD_VALUE;
        int channels=channelIndexMask!=0?Integer.bitCount(channelIndexMask):AudioFormat.channelCountFromInChannelMask(channelMask);
        int rate=sampleRate[0]==0?48000:sampleRate[0];
        if(native_get_min_buff_size(rate,channels,format)<0||bytes<channels*2||bytes>4*1024*1024||bytes%(channels*2)!=0)return AudioRecord.ERROR_BAD_VALUE;
        long token=MediaInputs.audioOpen(rate,channels);if(token==0)return AudioRecord.ERROR;
        HostMediaObjects.attach(owner,new Capture(token,channels,bytes,reference));sampleRate[0]=rate;return AudioRecord.SUCCESS;
    }
    public static void native_release(AudioRecord owner){HostMediaObjects.release(owner);}
    public static void native_finalize(AudioRecord owner){HostMediaObjects.release(owner);}
    public static int native_start(AudioRecord owner,int syncEvent,int session){if(syncEvent!=0)return AudioRecord.ERROR_INVALID_OPERATION;Capture c=capture(owner);synchronized(c){if(c.token==0||!MediaInputs.audioStart(c.token))return AudioRecord.ERROR;c.running=true;c.frames=0;c.marked=false;c.nextPeriod=c.period;return 0;}}
    public static void native_stop(AudioRecord owner){Capture c=capture(owner);synchronized(c){c.running=false;if(c.token!=0)MediaInputs.audioStop(c.token);}}
    public static int native_get_min_buff_size(int rate,int channels,int format){if(rate<4000||rate>192000||channels<1||channels>2||format!=AudioFormat.ENCODING_PCM_16BIT)return AudioRecord.ERROR_BAD_VALUE;return Math.max(256,rate/50)*channels*2;}
    public static int native_get_buffer_size_in_frames(AudioRecord owner){Capture c=capture(owner);return c.bufferBytes/(c.channels*2);}
    public static int native_read_in_short_array(AudioRecord owner,short[] data,int offset,int count,boolean blocking){Capture c=capture(owner);int done=0,limit=count-count%c.channels;while(done<limit){int received=c.read(data,offset+done,Math.min(limit-done,8192));if(received<0)return done==0?received:done;done+=received;if(!blocking)break;if(received==0)try{Thread.sleep(2);}catch(InterruptedException error){Thread.currentThread().interrupt();break;}}return done;}
    public static int native_read_in_byte_array(AudioRecord owner,byte[] data,int offset,int count,boolean blocking){short[] scratch=new short[Math.min(count/2,8192)];int done=0;while(done+2<=count){int read=native_read_in_short_array(owner,scratch,0,Math.min(scratch.length,(count-done)/2),blocking);if(read<0)return done==0?read:done;if(read==0)break;for(int i=0;i<read;i++){data[offset+done++]=(byte)scratch[i];data[offset+done++]=(byte)(scratch[i]>>>8);}if(!blocking)break;}return done;}
    public static int native_read_in_direct_buffer(AudioRecord owner,Object value,int count,boolean blocking){if(!(value instanceof ByteBuffer data)||!data.isDirect()||data.isReadOnly())return AudioRecord.ERROR_BAD_VALUE;ByteBuffer target=data.duplicate();target.clear();int limit=Math.min(count,target.capacity()),done=0;byte[] scratch=new byte[Math.min(limit,16384)];while(done<limit){int read=native_read_in_byte_array(owner,scratch,0,Math.min(scratch.length,limit-done),blocking);if(read<0)return done==0?read:done;if(read==0)break;target.put(scratch,0,read);done+=read;if(!blocking)break;}return done;}
    public static int native_read_in_float_array(AudioRecord owner,float[] data,int offset,int count,boolean blocking){capture(owner);return AudioRecord.ERROR_INVALID_OPERATION;}
    public static int native_set_marker_pos(AudioRecord owner,int marker){if(marker<0)return AudioRecord.ERROR_BAD_VALUE;Capture c=capture(owner);synchronized(c){c.marker=marker;c.marked=false;return 0;}}
    public static int native_get_marker_pos(AudioRecord owner){Capture c=capture(owner);synchronized(c){return c.marker;}}
    public static int native_set_pos_update_period(AudioRecord owner,int period){if(period<0)return AudioRecord.ERROR_BAD_VALUE;Capture c=capture(owner);synchronized(c){c.period=period;c.nextPeriod=c.frames+period;return 0;}}
    public static int native_get_pos_update_period(AudioRecord owner){Capture c=capture(owner);synchronized(c){return c.period;}}
    public static PersistableBundle native_getMetrics(AudioRecord owner){Capture c=capture(owner);PersistableBundle metrics=new PersistableBundle();metrics.putInt("android.media.audiorecord.channels",c.channels);return metrics;}
    public static int native_get_timestamp(AudioRecord owner,AudioTimestamp timestamp,int timebase){capture(owner);return AudioRecord.ERROR_INVALID_OPERATION;}
    public static int native_get_active_microphones(AudioRecord owner,ArrayList<MicrophoneInfo> microphones){capture(owner);return AudioRecord.ERROR_INVALID_OPERATION;}
    public static int[] native_getRoutedDeviceIds(AudioRecord owner){capture(owner);return new int[0];}
    public static boolean native_setInputDevice(AudioRecord owner,int device){capture(owner);return device==0;}
}
