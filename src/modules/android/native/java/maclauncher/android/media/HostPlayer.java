package maclauncher.android.media;
import android.media.*;
import maclauncher.android.media.AudioHost;
import java.io.FileDescriptor;
import java.io.IOException;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ThreadPoolExecutor;

final class HostPlayer implements HostMediaObjects.State {
    public interface OnCompletionListener { void onCompletion(HostPlayer player); }
    private enum State { IDLE, SOURCE, PREPARING, PREPARED, STARTED, PAUSED, STOPPED, COMPLETE, RELEASED }
    private final Handler events;
    private final AudioHost.Ownership handles=new AudioHost.Ownership();
    private final java.lang.ref.Cleaner.Cleanable cleanup=AudioHost.track(this,handles);
    private final Runnable completionPoll=this::poll;
    private State state=State.IDLE;
    private int session;
    public synchronized int session(){return session;}
    public synchronized void session(int value){live();if(value<0)throw new IllegalArgumentException("Invalid audio session");session=value;}
    private String path;private ParcelFileDescriptor descriptor;private long offset,size,peer;
    private boolean looping;private float left=1,right=1;private int streamType=AudioManager.STREAM_MUSIC;
    private OnCompletionListener listener;
    private Runnable chainedStart;
    private long sourceGeneration;
    private record Next(java.lang.ref.WeakReference<HostPlayer> player,long generation){}
    private Next next;
    private HostAudioRoute route;
    synchronized void routing(Runnable changed,boolean enabled){live();if(route==null)route=new HostAudioRoute(changed);route.enabled(enabled);}
    private FutureTask<Void> preparation;
    private ThreadPoolExecutor preparationExecutor;
    private long preparationGeneration;
    private boolean action(int action){try{return AudioHost.action(peer,action);}finally{java.lang.ref.Reference.reachabilityFence(this);}}
    private void configure(){configure(peer);}
    private void configure(long handle){try{AudioHost.setStream(handle,streamType);AudioHost.configure(handle,left,right,looping?-1:0,1);}finally{java.lang.ref.Reference.reachabilityFence(this);}}
    public HostPlayer(){Looper current=Looper.myLooper();events=new Handler(current!=null?current:Looper.getMainLooper());}
    private void live(){if(state==State.RELEASED)throw new IllegalStateException("MediaPlayer is released");}
    public synchronized void setAudioAttributes(AudioAttributes value){setAudioStreamType(AudioAttributes.toLegacyStreamType(java.util.Objects.requireNonNull(value)));}
    public synchronized void setAudioStreamType(int stream){live();streamType=maclauncher.android.media.HostAudioManager.checkedStream(stream);if(peer!=0)configure();}
    public synchronized void setDataSource(String source)throws IOException{if(state!=State.IDLE)throw new IllegalStateException("MediaPlayer already has a source");path=java.util.Objects.requireNonNull(source);size=-1;state=State.SOURCE;}
    public synchronized void setDataSource(FileDescriptor fd,long start,long length)throws IOException{if(state!=State.IDLE)throw new IllegalStateException("MediaPlayer already has a source");if(!fd.valid()||start<0||length<1)throw new IOException("Invalid audio file descriptor range");
        // Retain the actual source, not decoded media. Original prepare/prepareAsync
        // select which thread performs I/O and decoding after this call returns.
        descriptor=handles.add(ParcelFileDescriptor.dup(fd));offset=start;size=length;state=State.SOURCE;
    }
    private final class Preparation implements AutoCloseable {
        final long generation,offset,size,previous;final String path;final ParcelFileDescriptor descriptor;
        private boolean running,closed;
        Preparation(long generation,String path,ParcelFileDescriptor descriptor,long offset,long size,long previous){this.generation=generation;this.path=path;this.descriptor=descriptor;this.offset=offset;this.size=size;this.previous=previous;}
        synchronized boolean begin(){if(closed)return false;running=true;return true;}
        void cancelBeforeRun(){synchronized(this){if(running||closed)return;closed=true;}dispose();}
        @Override public void close(){synchronized(this){if(closed)return;closed=true;}dispose();}
        private void dispose(){if(descriptor!=null)try{descriptor.close();}catch(IOException ignored){}handles.release(previous);}
    }
    private synchronized Preparation beginPreparation()throws IOException{
        live();if(preparation!=null||state!=State.SOURCE&&state!=State.STOPPED)throw new IllegalStateException("MediaPlayer has no pending source");
        ParcelFileDescriptor source=descriptor==null?null:descriptor.dup();
        Preparation request=new Preparation(++preparationGeneration,path,source,offset,size,peer);peer=0;state=State.PREPARING;return request;
    }
    private boolean preparing(Preparation request){return preparationGeneration==request.generation&&state==State.PREPARING;}
    private boolean prepare(Preparation request)throws IOException{
        long loaded=0;
        try(request){
            if(!request.begin())return false;
            handles.release(request.previous);
            synchronized(this){if(!preparing(request))return false;}
            loaded=AudioHost.loadMedia(request.path,request.descriptor==null?null:request.descriptor.getFileDescriptor(),request.offset,request.size);
            synchronized(this){if(!preparing(request))return false;configure(loaded);}
            if(!AudioHost.action(loaded,5))throw new IOException("Audio output could not prepare");
            synchronized(this){if(!preparing(request))return false;configure(loaded);peer=handles.add(loaded);loaded=0;state=State.PREPARED;return true;}
        }catch(IOException|RuntimeException|Error failure){synchronized(this){if(preparing(request))state=State.SOURCE;}throw failure;}
        finally{if(loaded!=0)AudioHost.action(loaded,3);java.lang.ref.Reference.reachabilityFence(this);}
    }
    public void prepare()throws IOException{if(!prepare(beginPreparation()))throw new IOException("Media preparation was cancelled");}
    synchronized void prepareAsync(ThreadPoolExecutor executor,Runnable prepared,Runnable failed){
        Preparation request;try{request=beginPreparation();}catch(IOException failure){throw new java.io.UncheckedIOException(failure);}
        FutureTask<Void> task=new FutureTask<>(()->{
            try{
                boolean ready=prepare(request);
                synchronized(HostPlayer.this){if(ready&&request.generation==preparationGeneration)prepared.run();}
            }catch(IOException|RuntimeException error){synchronized(HostPlayer.this){if(request.generation==preparationGeneration&&state!=State.RELEASED)failed.run();}}
            catch(Error error){
                Thread worker=Thread.currentThread();worker.getUncaughtExceptionHandler().uncaughtException(worker,error);
                synchronized(HostPlayer.this){if(request.generation==preparationGeneration&&state!=State.RELEASED)failed.run();}
            }
            finally{synchronized(HostPlayer.this){if(request.generation==preparationGeneration){preparation=null;preparationExecutor=null;}}}
            return null;
        }){
            @Override protected void done(){
                // A task removed before execution still owns its source duplicate.
                if(isCancelled())request.cancelBeforeRun();
            }
        };
        preparation=task;preparationExecutor=executor;
        try{executor.execute(task);}catch(RuntimeException error){cancelPreparation();state=State.SOURCE;throw error;}
    }
    private void cancelPreparation(){
        preparationGeneration++;
        if(preparation!=null){preparation.cancel(false);preparationExecutor.remove(preparation);preparation=null;preparationExecutor=null;}
    }
    public synchronized void start(){live();if(state==State.STARTED)return;if(state!=State.PREPARED&&state!=State.PAUSED&&state!=State.COMPLETE)throw new IllegalStateException("MediaPlayer is not prepared");if(!action(0))throw new IllegalStateException("Audio output could not start");state=State.STARTED;cancelEvent();events.postDelayed(completionPoll,20);}
    synchronized void onChainedStart(Runnable callback){live();chainedStart=callback;}
    void next(HostPlayer target){
        if(target==this)throw new IllegalArgumentException("A MediaPlayer cannot follow itself");
        Next link=null;if(target!=null)synchronized(target){target.live();if(target.state!=State.PREPARED&&target.state!=State.PAUSED&&target.state!=State.COMPLETE)throw new IllegalStateException("Next MediaPlayer is not prepared");link=new Next(new java.lang.ref.WeakReference<>(target),target.sourceGeneration);}
        synchronized(this){live();next=link;}
    }
    private void startChained(long generation){Runnable callback;synchronized(this){if(sourceGeneration!=generation||state==State.RELEASED||state==State.IDLE||state==State.SOURCE||state==State.PREPARING||state==State.STOPPED)return;start();callback=chainedStart;}if(callback!=null)callback.run();}
    private void poll(){OnCompletionListener callback=null;Next link=null;synchronized(this){if(state!=State.STARTED)return;if(!looping&&!action(4)){state=State.COMPLETE;callback=listener;link=next;}else events.postDelayed(completionPoll,20);}if(link!=null){HostPlayer target=link.player().get();if(target!=null)target.startChained(link.generation());}if(callback!=null)callback.onCompletion(this);}
    private void cancelEvent(){events.removeCallbacks(completionPoll);}
    public synchronized void pause(){live();if(state==State.STARTED){action(1);state=State.PAUSED;cancelEvent();}}
    public synchronized void stop(){live();if(peer==0)throw new IllegalStateException("MediaPlayer is not prepared");action(2);state=State.STOPPED;cancelEvent();}
    @Override public void close(){release();}
    public void reset(){long retired;ParcelFileDescriptor source;synchronized(this){live();sourceGeneration++;next=null;cancelPreparation();cancelEvent();retired=peer;source=descriptor;peer=0;descriptor=null;path=null;looping=false;state=State.IDLE;}handles.release(retired);if(source!=null)handles.release(source);}
    public synchronized int streamType(){return streamType;}
    public synchronized int duration(){live();if(peer==0)throw new IllegalStateException("MediaPlayer is not prepared");return AudioHost.time(peer,false);}
    public synchronized int position(){live();if(peer==0)throw new IllegalStateException("MediaPlayer is not prepared");return AudioHost.time(peer,true);}
    public synchronized void seek(long millis){live();if(peer==0)throw new IllegalStateException("MediaPlayer is not prepared");AudioHost.seek(peer,millis);}
    public void release(){HostAudioRoute retired;synchronized(this){if(state==State.RELEASED)return;sourceGeneration++;next=null;cancelPreparation();cancelEvent();retired=route;route=null;peer=0;descriptor=null;path=null;listener=null;chainedStart=null;state=State.RELEASED;}if(retired!=null)retired.close();cleanup.clean();}
    public synchronized boolean isPlaying(){live();return peer!=0&&state==State.STARTED&&action(4);}
    public synchronized boolean isLooping(){live();return looping;}
    public synchronized void setLooping(boolean value){live();looping=value;if(peer!=0)configure();}
    public synchronized void setVolume(float l,float r){live();if(!Float.isFinite(l)||!Float.isFinite(r)||l<0||l>1||r<0||r>1)throw new IllegalArgumentException("Volume must be within 0 and 1");left=l;right=r;if(peer!=0)configure();}
    public synchronized void setOnCompletionListener(OnCompletionListener value){live();listener=value;}
}
