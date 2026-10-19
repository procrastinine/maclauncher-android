package maclauncher.android.media;

import android.media.*;
import android.os.*;
import java.lang.reflect.*;
import java.nio.*;
import java.util.concurrent.atomic.AtomicInteger;

/** AudioTrack's PCM device boundary. Public validation, player registration and callbacks remain AOSP. */
public final class HostAudioTrack {
    private HostAudioTrack(){}
    private static final int RATE=48000,BURST=240;
    private static final AtomicInteger sessions=new AtomicInteger(1);
    private static final Method EVENT;
    static {AudioHost.initialize();try{EVENT=AudioTrack.class.getDeclaredMethod("postEventFromNative",Object.class,int.class,int.class,int.class,Object.class);EVENT.setAccessible(true);}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    private static native long sinkOpen(int channels,int rate);
    private static native int sinkWrite(long handle,float[] pcm,int offset,int frames);
    private static native void sinkControls(long handle,double rate,float left,float right,int dualMono);
    private static native void sinkState(long handle,int state);
    private static native long sinkPosition(long handle);
    private static native long sinkQueued(long handle);
    private static native int sinkUnderruns(long handle);
    private static native void sinkClose(long handle);
    private static native int sinkApplyShaper(long handle,VolumeShaper.Configuration configuration,VolumeShaper.Operation operation);
    private static native VolumeShaper.State sinkShaperState(long handle,int id);
    private static void event(Object reference,int type){try{EVENT.invoke(null,reference,type,0,0,null);}catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}catch(ReflectiveOperationException error){throw new IllegalStateException(error);}}
    private static int bytes(int format){return switch(format){case AudioFormat.ENCODING_PCM_8BIT->1;case AudioFormat.ENCODING_PCM_16BIT->2;case AudioFormat.ENCODING_PCM_FLOAT,AudioFormat.ENCODING_PCM_32BIT->4;case AudioFormat.ENCODING_PCM_24BIT_PACKED->3;default->0;};}
    private static final class Track implements HostMediaObjects.State {
        final int channels,format,capacity,mode,sampleRate;final Object reference;final float[] data;final Thread worker;final HostAudioRoute route;
        long sink,read,write,generation,basePosition;int size,rate,marker,period,nextPeriod,loopStart,loopEnd,loops,startThreshold,playerId,dualMono;
        float left=1,right=1,descriptionDb=Float.NEGATIVE_INFINITY;boolean playing,closed,marked,failed,buffering=true;PlaybackParams params=new PlaybackParams().allowDefaults();
        Track(int channels,int format,int capacity,int mode,int rate,Object reference){this.channels=channels;this.format=format;this.capacity=capacity;this.size=capacity;this.mode=mode;this.sampleRate=this.rate=rate;this.reference=reference;route=new HostAudioRoute(()->event(reference,1000));data=new float[Math.multiplyExact(capacity,channels)];sink=sinkOpen(channels,rate);if(sink==0)throw new IllegalStateException("Audio output cannot be opened");worker=new Thread(this::feed,"Android PCM output");worker.setDaemon(true);worker.start();}
        public void close(){synchronized(this){if(closed)return;closed=true;playing=false;generation++;if(sink!=0)sinkState(sink,3);route.close();notifyAll();}if(Thread.currentThread()!=worker){boolean interrupted=false;for(;;)try{worker.join();break;}catch(InterruptedException e){interrupted=true;}if(interrupted)Thread.currentThread().interrupt();}synchronized(this){if(sink!=0){sinkClose(sink);sink=0;}}}
        synchronized int put(float[] source,int offset,int count,boolean blocking){int done=0;while(done<count&&!closed&&!failed){int free=mode==AudioTrack.MODE_STATIC?capacity-(int)write:size-(int)(write-read);if(free<=0){if(!blocking||mode==AudioTrack.MODE_STATIC)break;try{wait();}catch(InterruptedException e){Thread.currentThread().interrupt();break;}continue;}int take=Math.min(count-done,free);int at=(int)(write%capacity),run=Math.min(take,capacity-at);System.arraycopy(source,offset+done*channels,data,at*channels,run*channels);if(run<take)System.arraycopy(source,offset+(done+run)*channels,data,0,(take-run)*channels);write+=take;done+=take;notifyAll();}return (closed||failed)&&done==0?AudioTrack.ERROR_DEAD_OBJECT:done;}
        synchronized int position(){return sink==0?0:(int)(basePosition+sinkPosition(sink));}
        void controls(){sinkControls(sink,rate*(double)params.getSpeed(),left,right,dualMono);}
        void feed(){
            float[] output=new float[Math.multiplyExact(BURST,channels)];
            try{while(true){
                boolean mark=false,periodic=false;int observedPosition;
                synchronized(this){
                    if(closed)return;
                    int position=observedPosition=position();
                    if(marker>0&&!marked&&Integer.compareUnsigned(position,marker)>=0){marked=true;mark=true;}
                    if(period>0&&Integer.compareUnsigned(position,nextPeriod)>=0){nextPeriod=position+period;periodic=true;}
                }
                if(mark)event(reference,3);if(periodic)event(reference,4);
                int produced=0;long token,epoch;
                synchronized(this){
                    while(!closed&&(!playing||params.getSpeed()==0||buffering&&write-read<Math.min(mode==AudioTrack.MODE_STATIC?write:capacity,startThreshold==0?capacity:startThreshold)))wait();
                    if(closed)return;
                    buffering=false;
                    if(write<=read){
                        // Keep position callbacks alive while the native queue drains;
                        // no polling remains once all source frames are consumed.
                        if(sinkQueued(sink)>0)wait(5);else if(position()==observedPosition)wait();
                        continue;
                    }
                    epoch=generation;
                    while(produced<BURST&&write>read){
                        long end=write;
                        if(mode==AudioTrack.MODE_STATIC&&loops!=0&&loopEnd>loopStart)end=Math.min(end,loopEnd);
                        int at=(int)(read%capacity),take=(int)Math.min(Math.min(BURST-produced,end-read),capacity-at);
                        if(take>0){System.arraycopy(data,at*channels,output,produced*channels,take*channels);read+=take;produced+=take;}
                        if(mode==AudioTrack.MODE_STATIC&&loops!=0&&loopEnd>loopStart&&read>=loopEnd){read=loopStart+(read-loopEnd);if(loops>0)loops--;}
                        else if(take==0)break;
                    }
                    token=sink;notifyAll();
                }
                int sent=0;
                while(sent<produced){
                    synchronized(this){while(!closed&&epoch==generation&&!playing)wait();if(closed)return;if(epoch!=generation)break;}
                    int accepted=sinkWrite(token,output,sent,produced-sent);
                    if(accepted<0){synchronized(this){playing=false;failed=true;notifyAll();}System.err.println("[android-audio] PCM output stopped ("+accepted+")");break;}
                    sent+=accepted;
                }
            }}catch(InterruptedException ignored){Thread.currentThread().interrupt();}
            catch(Throwable error){synchronized(this){playing=false;failed=true;notifyAll();}System.err.println("[android-audio] PCM output failed: "+error);}
        }
    }
    private static Track track(AudioTrack owner){return HostMediaObjects.get(owner,Track.class);}
    public static boolean native_is_direct_output_supported(int encoding,int rate,int mask,int indexMask,int content,int usage,int flags){return false;}
    public static int native_setup(AudioTrack owner,Object reference,Object attributes,int[] sampleRate,int mask,int indexMask,int format,int bufferBytes,int mode,int[] session,Parcel attribution,long existing,boolean offload,int encapsulation,Object tuner,String packageName){
        if(existing!=0||offload||encapsulation!=0)return AudioTrack.ERROR_INVALID_OPERATION;
        if(!(attributes instanceof AudioAttributes)||sampleRate==null||sampleRate.length==0||session==null||session.length==0)return AudioTrack.ERROR_BAD_VALUE;
        int channels=indexMask!=0?Integer.bitCount(indexMask):AudioFormat.channelCountFromOutChannelMask(mask),width=bytes(format),rate=sampleRate[0]==0?RATE:sampleRate[0];
        if(channels<1||width==0||rate<=0||bufferBytes<channels*width||bufferBytes%(channels*width)!=0)return AudioTrack.ERROR_BAD_VALUE;
        Track state=new Track(channels,format,bufferBytes/(channels*width),mode,rate,reference);HostMediaObjects.attach(owner,state);sampleRate[0]=rate;if(session[0]==0)session[0]=sessions.getAndUpdate(value->value==Integer.MAX_VALUE?1:value+1);return AudioTrack.SUCCESS;
    }
    public static void native_finalize(AudioTrack owner){HostMediaObjects.release(owner);}
    public static void native_release(AudioTrack owner){HostMediaObjects.release(owner);}
    public static void native_start(AudioTrack owner){Track t=track(owner);synchronized(t){sinkState(t.sink,0);t.playing=true;t.notifyAll();}}
    public static void native_pause(AudioTrack owner){Track t=track(owner);synchronized(t){t.playing=false;sinkState(t.sink,1);}}
    public static void native_stop(AudioTrack owner){Track t=track(owner);synchronized(t){t.playing=false;t.generation++;sinkState(t.sink,3);t.basePosition=0;t.buffering=true;t.read=t.mode==AudioTrack.MODE_STATIC?0:t.write;t.marked=false;t.nextPeriod=t.period;t.notifyAll();}}
    public static void native_flush(AudioTrack owner){Track t=track(owner);synchronized(t){if(t.mode==AudioTrack.MODE_STREAM&&!t.playing){t.generation++;sinkState(t.sink,2);t.basePosition=0;t.buffering=true;t.read=t.write=0;t.notifyAll();}}}
    private static float sample(ByteBuffer data,int format){return switch(format){case AudioFormat.ENCODING_PCM_8BIT->((data.get()&255)-128)/128f;case AudioFormat.ENCODING_PCM_16BIT->data.getShort()/32768f;case AudioFormat.ENCODING_PCM_FLOAT->data.getFloat();case AudioFormat.ENCODING_PCM_32BIT->(float)(data.getInt()/2147483648.0);case AudioFormat.ENCODING_PCM_24BIT_PACKED->{int value=(data.get()&255)|((data.get()&255)<<8)|(data.get()<<16);yield value/8388608f;}default->throw new IllegalArgumentException("Not a PCM encoding");};}
    private static int writeBytes(Track t,ByteBuffer data,int count,int format,boolean blocking){int width=bytes(format);if(width==0)return AudioTrack.ERROR_BAD_VALUE;int frames=count/(width*t.channels),done=0;float[] values=new float[Math.min(frames,BURST*4)*t.channels];while(done<frames){int take=Math.min(frames-done,values.length/t.channels);for(int i=0;i<take*t.channels;i++)values[i]=sample(data,format);int accepted=t.put(values,0,take,blocking);if(accepted<0)return done==0?accepted:done*width*t.channels;done+=accepted;if(accepted<take||!blocking)break;}return done*width*t.channels;}
    public static int native_write_byte(AudioTrack owner,byte[] data,int offset,int count,int format,boolean blocking){return writeBytes(track(owner),ByteBuffer.wrap(data,offset,count).order(ByteOrder.nativeOrder()),count,format,blocking);}
    public static int native_write_native_bytes(AudioTrack owner,ByteBuffer data,int offset,int count,int format,boolean blocking){ByteBuffer copy=data.duplicate().order(ByteOrder.nativeOrder());copy.clear().position(offset).limit(offset+count);return writeBytes(track(owner),copy,count,format,blocking);}
    public static int native_write_short(AudioTrack owner,short[] data,int offset,int count,int format,boolean blocking){Track t=track(owner);int done=0,frames=count/t.channels;float[] values=new float[Math.min(frames,BURST*4)*t.channels];while(done<frames){int take=Math.min(frames-done,values.length/t.channels);for(int i=0;i<take*t.channels;i++)values[i]=data[offset+done*t.channels+i]/32768f;int accepted=t.put(values,0,take,blocking);if(accepted<0)return done==0?accepted:done*t.channels;done+=accepted;if(accepted<take||!blocking)break;}return done*t.channels;}
    public static int native_write_float(AudioTrack owner,float[] data,int offset,int count,int format,boolean blocking){Track t=track(owner);int result=t.put(data,offset,count/t.channels,blocking);return result<0?result:result*t.channels;}
    public static int native_reload_static(AudioTrack owner){Track t=track(owner);synchronized(t){if(t.mode!=AudioTrack.MODE_STATIC||t.playing)return AudioTrack.ERROR_INVALID_OPERATION;t.generation++;sinkState(t.sink,2);t.basePosition=0;t.read=0;t.notifyAll();return 0;}}
    public static int native_get_buffer_size_frames(AudioTrack owner){return track(owner).size;}
    public static int native_set_buffer_size_frames(AudioTrack owner,int frames){Track t=track(owner);synchronized(t){if(frames<0)return AudioTrack.ERROR_BAD_VALUE;t.size=Math.max(1,Math.min(frames,t.capacity));t.notifyAll();return t.size;}}
    public static int native_get_buffer_capacity_frames(AudioTrack owner){return track(owner).capacity;}
    public static void native_setVolume(AudioTrack owner,float left,float right){Track t=track(owner);synchronized(t){t.left=left;t.right=right;t.controls();}}
    public static int native_set_playback_rate(AudioTrack owner,int rate){if(rate<=0)return AudioTrack.ERROR_BAD_VALUE;Track t=track(owner);synchronized(t){t.rate=rate;t.controls();return 0;}}
    public static int native_get_playback_rate(AudioTrack owner){return track(owner).rate;}
    public static void native_set_playback_params(AudioTrack owner,PlaybackParams params){Track t=track(owner);synchronized(t){if(params.getPitch()!=1)throw new IllegalArgumentException("Independent pitch adjustment is unavailable");if(params.getSpeed()<0)throw new IllegalArgumentException("Negative playback speed");t.params=new PlaybackParams().allowDefaults().setSpeed(params.getSpeed()).setPitch(params.getPitch()).setAudioFallbackMode(params.getAudioFallbackMode());t.controls();if(params.getSpeed()==0){t.playing=false;sinkState(t.sink,1);}t.notifyAll();}}
    public static PlaybackParams native_get_playback_params(AudioTrack owner){Track t=track(owner);synchronized(t){return new PlaybackParams().allowDefaults().setSpeed(t.params.getSpeed()).setPitch(t.params.getPitch()).setAudioFallbackMode(t.params.getAudioFallbackMode());}}
    public static int native_set_marker_pos(AudioTrack owner,int value){Track t=track(owner);synchronized(t){t.marker=value;t.marked=false;return 0;}}
    public static int native_get_marker_pos(AudioTrack owner){return track(owner).marker;}
    public static int native_set_pos_update_period(AudioTrack owner,int value){if(value<0)return AudioTrack.ERROR_BAD_VALUE;Track t=track(owner);synchronized(t){t.period=value;t.nextPeriod=t.position()+value;return 0;}}
    public static int native_get_pos_update_period(AudioTrack owner){return track(owner).period;}
    public static int native_set_position(AudioTrack owner,int frame){Track t=track(owner);synchronized(t){if(t.mode!=AudioTrack.MODE_STATIC||frame<0||frame>t.write||t.playing)return AudioTrack.ERROR_INVALID_OPERATION;t.generation++;sinkState(t.sink,2);t.basePosition=frame;t.read=frame;return 0;}}
    public static int native_get_position(AudioTrack owner){return track(owner).position();}
    public static int native_get_latency(AudioTrack owner){Track t=track(owner);synchronized(t){return (int)(sinkQueued(t.sink)*1000/Math.max(1,t.rate*(double)t.params.getSpeed()));}}
    public static int native_get_underrun_count(AudioTrack owner){Track t=track(owner);synchronized(t){return sinkUnderruns(t.sink);}}
    public static int native_get_flags(AudioTrack owner){track(owner);return 0;}
    public static int native_get_timestamp(AudioTrack owner,long[] values){Track t=track(owner);synchronized(t){if(!t.playing||t.position()==0)return AudioTrack.ERROR_INVALID_OPERATION;values[0]=Integer.toUnsignedLong(t.position());values[1]=System.nanoTime();return 0;}}
    public static int native_set_loop(AudioTrack owner,int start,int end,int loops){Track t=track(owner);synchronized(t){if(t.mode!=AudioTrack.MODE_STATIC)return AudioTrack.ERROR_INVALID_OPERATION;if(start<0||end<=start||end>t.write||loops< -1)return AudioTrack.ERROR_BAD_VALUE;t.loopStart=start;t.loopEnd=end;t.loops=loops;return 0;}}
    public static int native_get_output_sample_rate(int stream){return RATE;}
    public static int native_get_min_buff_size(int rate,int channels,int format){int width=bytes(format);if(rate<=0||channels<=0||width==0)return AudioTrack.ERROR_BAD_VALUE;long size=Math.max(BURST,((long)rate+49)/50)*channels*width;return size>Integer.MAX_VALUE?AudioTrack.ERROR_BAD_VALUE:(int)size;}
    public static int native_attachAuxEffect(AudioTrack owner,int id){track(owner);return id==0?0:AudioTrack.ERROR_INVALID_OPERATION;}
    public static int native_setAuxEffectSendLevel(AudioTrack owner,float level){track(owner);return level==0?0:AudioTrack.ERROR_INVALID_OPERATION;}
    public static boolean native_setOutputDevice(AudioTrack owner,int id){track(owner);return HostAudioRoute.select(id);}
    public static int[] native_getRoutedDeviceIds(AudioTrack owner){track(owner);return HostAudioRoute.devices();}
    public static void native_enableDeviceCallback(AudioTrack owner){track(owner).route.enabled(true);}
    public static void native_disableDeviceCallback(AudioTrack owner){track(owner).route.enabled(false);}
    public static int native_applyVolumeShaper(AudioTrack owner,VolumeShaper.Configuration configuration,VolumeShaper.Operation operation){Track t=track(owner);synchronized(t){return sinkApplyShaper(t.sink,configuration,operation);}}
    public static VolumeShaper.State native_getVolumeShaperState(AudioTrack owner,int id){Track t=track(owner);synchronized(t){return sinkShaperState(t.sink,id);}}
    public static int native_setPresentation(AudioTrack owner,int presentation,int program){track(owner);return AudioTrack.ERROR_INVALID_OPERATION;}
    public static int native_getPortId(AudioTrack owner){Track t=track(owner);return t.playerId;}
    public static void native_set_delay_padding(AudioTrack owner,int delay,int padding){track(owner);if(delay!=0||padding!=0)throw new IllegalStateException("PCM output does not use compressed offload padding");}
    public static int native_set_audio_description_mix_level_db(AudioTrack owner,float level){Track t=track(owner);synchronized(t){if(level!=Float.NEGATIVE_INFINITY)return AudioTrack.ERROR_INVALID_OPERATION;t.descriptionDb=level;return 0;}}
    public static int native_get_audio_description_mix_level_db(AudioTrack owner,float[] values){values[0]=track(owner).descriptionDb;return 0;}
    public static int native_set_dual_mono_mode(AudioTrack owner,int mode){Track t=track(owner);synchronized(t){t.dualMono=mode;t.controls();return 0;}}
    public static int native_get_dual_mono_mode(AudioTrack owner,int[] values){values[0]=track(owner).dualMono;return 0;}
    public static void native_setLogSessionId(AudioTrack owner,String id){track(owner);}
    public static int native_setStartThresholdInFrames(AudioTrack owner,int frames){Track t=track(owner);synchronized(t){if(frames<=0)return AudioTrack.ERROR_BAD_VALUE;t.startThreshold=Math.min(frames,t.capacity);t.notifyAll();return t.startThreshold;}}
    public static int native_getStartThresholdInFrames(AudioTrack owner){Track t=track(owner);return t.startThreshold==0?t.capacity:t.startThreshold;}
    public static void native_setPlayerIId(AudioTrack owner,int id){track(owner).playerId=id;}
    public static PersistableBundle native_getMetrics(AudioTrack owner){Track t=track(owner);PersistableBundle result=new PersistableBundle();result.putInt("android.media.audiotrack.samplerate",t.sampleRate);result.putInt("android.media.audiotrack.channelcount",t.channels);return result;}
}
