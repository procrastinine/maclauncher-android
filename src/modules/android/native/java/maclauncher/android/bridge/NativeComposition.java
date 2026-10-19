package maclauncher.android.bridge;

import android.os.*;
import android.system.*;
import android.view.SurfaceControl;
import android.view.DisplayEventReceiver;
import android.graphics.Rect;
import android.graphics.Color;
import java.io.IOException;
import java.nio.*;
import java.nio.channels.FileChannel;
import java.util.*;
import java.util.concurrent.locks.ReentrantLock;
import maclauncher.android.framework.HostSurface;
import maclauncher.android.framework.HostNio;
import maclauncher.android.framework.HostBinderRpc;
import maclauncher.android.framework.HostDisplayEvents;

/** Per-layer producers over the original native composition buffer owner.
 * The server runs beside original SurfaceControl; clients retain FD capabilities
 * through the existing Binder RPC transport. Loading the class starts nothing. */
public final class NativeComposition {
    private static final String ROOT="maclauncher.android.NativeComposition",LAYER=ROOT+".Layer";
    private static final int OPEN=IBinder.FIRST_CALL_TRANSACTION,DEQUEUE=OPEN,QUEUE=OPEN+1,CANCEL=OPEN+2,STATE=OPEN+3,RESIZE=OPEN+4,CLOSE=OPEN+5;
    private static final int AVAILABLE=OPEN+1,RETIRED=OPEN+2;
    private static final int CONFIGURE=OPEN+1;
    private static final int VSYNC=OPEN+2;
    private static final int ORIENTATION=OPEN+3;
    private NativeComposition(){}
    private static void dimensions(int width,int height){
        if(width<1||height<1)throw new IllegalArgumentException("Native surface dimensions must be positive");
        Math.multiplyExact(Math.multiplyExact((long)width,height),4L);
    }
    private static void close(ParcelFileDescriptor value){if(value!=null)try{value.close();}catch(IOException failure){throw new IllegalStateException(failure);}}
    private static final class Cancellation implements AutoCloseable {
        private final ParcelFileDescriptor read,write;private boolean closed;
        Cancellation(){try{var pipe=ParcelFileDescriptor.createPipe();read=pipe[0];write=pipe[1];}catch(IOException failure){throw new IllegalStateException("Native buffer cancellation is unavailable",failure);}}
        synchronized ParcelFileDescriptor borrow(){if(closed)throw new IllegalStateException("Native buffer operation was cancelled");try{return ParcelFileDescriptor.dup(read.getFileDescriptor());}catch(IOException failure){throw new IllegalStateException(failure);}}
        @Override public synchronized void close(){if(closed)return;closed=true;NativeComposition.close(write);NativeComposition.close(read);}
    }
    private static void awaitFence(ParcelFileDescriptor value,Cancellation cancellation){
        if(value==null)return;
        try(value;var cancel=cancellation.borrow()){var event=new StructPollfd();event.fd=value.getFileDescriptor();event.events=(short)OsConstants.POLLIN;
            var ended=new StructPollfd();ended.fd=cancel.getFileDescriptor();ended.events=(short)OsConstants.POLLIN;
            for(;;){try{Os.poll(new StructPollfd[]{event,ended},-1);break;}catch(ErrnoException failure){if(failure.errno!=OsConstants.EINTR)throw failure;}}
            if(ended.revents!=0)throw new IllegalStateException("Native buffer operation was cancelled");
            if((event.revents&OsConstants.POLLNVAL)!=0)throw new IllegalStateException("Native surface fence is invalid");
        }catch(IOException|ErrnoException failure){throw new IllegalStateException("Native surface fence failed",failure);}
    }
    /** Explicit original-native component entry; ordinary JVM windows keep their
     * existing output. The owning session must close this capability. */
    public static Server serve(SurfaceControl parent,String library){System.load(Objects.requireNonNull(library));return new Server(parent);}
    /** Authenticated launch capability; this session never starts a service or
     * creates a Mac window. The supervisor owns those physical resources. */
    public static final class Session implements AutoCloseable{
        private final HostBinderRpc.Connection connection;
        private volatile Handler clockOwner;private volatile boolean closed;
        private final java.util.concurrent.atomic.AtomicBoolean clockRequested=new java.util.concurrent.atomic.AtomicBoolean();
        private final Binder clock=new Binder(){@Override protected boolean onTransact(int code,Parcel request,Parcel reply,int flags)throws RemoteException{
            if(code!=VSYNC)return super.onTransact(code,request,reply,flags);request.enforceInterface(ROOT);long sequence=request.readLong(),callback=request.readLong(),output=request.readLong(),interval=request.readLong();request.enforceNoDataAvail();var handler=clockOwner;
            if(handler!=null)handler.post(()->{if(closed||clockOwner!=handler)return;HostDisplayEvents.hostDispatchFrame(sequence,callback,output,interval,screen);HostDisplayEvents.hostPresentFrame();signalClock();});return true;
        }};
        public final int width,height,densityDpi;public final long screen;public final float refresh;
        public Session(String socket,HostBinderRpc.Peer peer,org.json.JSONObject display)throws IOException,org.json.JSONException{
            width=display.getInt("width");height=display.getInt("height");densityDpi=display.getInt("densityDpi");screen=display.getLong("screen");refresh=(float)display.getDouble("refresh");dimensions(width,height);
            if(densityDpi<=0||!Float.isFinite(refresh)||refresh<=0)throw new IllegalArgumentException("Native display metadata is invalid");
            connection=HostBinderRpc.connect(socket,peer,4);
        }
        public IBinder root(){return connection.root();}
        public long apply(List<State> states,Layer completed){return NativeComposition.apply(root(),states,completed);}
        public void orientation(boolean landscape){var data=Parcel.obtain();var reply=Parcel.obtain();try{data.writeInterfaceToken(ROOT);data.writeBoolean(landscape);if(!root().transact(ORIENTATION,data,reply,0))throw new IllegalStateException("Native display orientation is unavailable");reply.readException();}catch(RemoteException failure){throw failure.rethrowAsRuntimeException();}finally{reply.recycle();data.recycle();}}
        private void signalClock(){var owner=clockOwner;if(closed||owner==null||!clockRequested.compareAndSet(false,true))return;
            owner.post(()->{clockRequested.set(false);if(closed||clockOwner!=owner||!HostDisplayEvents.hostNeedsFrames())return;var data=Parcel.obtain();try{data.writeInterfaceToken(ROOT);data.writeStrongBinder(clock);if(!root().transact(VSYNC,data,null,IBinder.FLAG_ONEWAY))throw new IllegalStateException("Native display event source is unavailable");}catch(RemoteException failure){throw failure.rethrowAsRuntimeException();}finally{data.recycle();}});
        }
        public void startClock(){if(clockOwner!=null)throw new IllegalStateException("Native display event source is already attached");clockOwner=new Handler(Looper.myLooper());HostDisplayEvents.hostSetWakeup(this::signalClock);}
        public void stopClock(){if(clockOwner==null)return;clockOwner=null;HostDisplayEvents.hostDetachSource();}
        @Override public void close(){closed=true;stopClock();connection.close();}
    }
    /** Original SurfaceControl state travels per layer; pixel composition never
     * runs in this adapter. Parent and relative references remain capabilities. */
    public record State(Layer layer,Layer parent,Layer relative,int flags,float x,float y,int z,float alpha,float dsdx,float dtdx,float dsdy,float dtdy,Rect crop,Rect destination,float cornerRadius,Integer color){}
    public static long apply(IBinder server,List<State> states,Layer completed){
        long frame=completed.reserveFrame();var request=Parcel.obtain();var reply=Parcel.obtain();
        try{request.writeInterfaceToken(ROOT);request.writeInt(states.size());for(var state:states){request.writeStrongBinder(state.layer.remote);request.writeStrongBinder(state.parent==null?null:state.parent.remote);request.writeStrongBinder(state.relative==null?null:state.relative.remote);request.writeInt(state.flags);request.writeFloat(state.x);request.writeFloat(state.y);request.writeInt(state.z);request.writeFloat(state.alpha);request.writeFloat(state.dsdx);request.writeFloat(state.dtdx);request.writeFloat(state.dsdy);request.writeFloat(state.dtdy);request.writeTypedObject(state.crop,0);request.writeTypedObject(state.destination,0);request.writeFloat(state.cornerRadius);request.writeBoolean(state.color!=null);if(state.color!=null)request.writeInt(state.color);}
            request.writeStrongBinder(completed.remote);request.writeLong(frame);if(!server.transact(CONFIGURE,request,reply,0))throw new IllegalStateException("Native surface transactions are unavailable");reply.readException();return frame;
        }catch(RemoteException failure){throw failure.rethrowAsRuntimeException();}finally{reply.recycle();request.recycle();}
    }
    public static final class Server extends Binder implements AutoCloseable {
        private long root;
        private final SurfaceControl parent;
        private final Set<ServerLayer> layers=new HashSet<>();
        private final Map<IBinder,Clock> clocks=new HashMap<>();private HandlerThread clockThread;private Handler clockHandler;private DisplayEventReceiver receiver;
        private final class Clock implements IBinder.DeathRecipient {final IBinder callback;boolean requested;Clock(IBinder callback){this.callback=callback;}@Override public void binderDied(){synchronized(Server.this){clocks.remove(callback);}}}
        private synchronized void schedule(IBinder callback)throws RemoteException{
            if(root==0)throw new IllegalStateException("Native composition is closed");var clock=clocks.get(callback);if(clock==null){clock=new Clock(callback);callback.linkToDeath(clock,0);clocks.put(callback,clock);}clock.requested=true;
            if(clockThread==null){clockThread=new HandlerThread("Android native display events");clockThread.setDaemon(true);clockThread.start();clockHandler=new Handler(clockThread.getLooper());}
            clockHandler.post(()->{synchronized(Server.this){if(root==0)return;if(receiver==null)receiver=new DisplayEventReceiver(Looper.myLooper()){@Override public void onVsync(long time,long display,int frame,VsyncEventData data){deliver(time,frame,data);}};receiver.scheduleVsync();}});
        }
        private void deliver(long time,int frame,DisplayEventReceiver.VsyncEventData data){List<Clock> pending;synchronized(this){pending=clocks.values().stream().filter(value->value.requested).toList();for(var value:pending)value.requested=false;}
            for(var value:pending){var request=Parcel.obtain();try{request.writeInterfaceToken(ROOT);request.writeLong(data.frameTimelines[data.preferredFrameTimelineIndex].vsyncId);request.writeLong(time);request.writeLong(data.frameTimelines[data.preferredFrameTimelineIndex].expectedPresentationTime);request.writeLong(data.frameInterval);value.callback.transact(VSYNC,request,null,IBinder.FLAG_ONEWAY);}catch(RemoteException gone){value.binderDied();}finally{request.recycle();}}
        }
        private Server(SurfaceControl parent){attachInterface(null,ROOT);this.parent=Objects.requireNonNull(parent);root=rootNative(parent);}
        private ServerLayer layer(IBinder capability){if(capability==null)return null;var local=capability.queryLocalInterface(LAYER);if(local==null||!(local.asBinder() instanceof ServerLayer layer)||layer.owner!=this)throw new SecurityException("Native surface belongs to another composition owner");layer.check();return layer;}
        @Override protected synchronized boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            if(code!=OPEN&&code!=CONFIGURE&&code!=VSYNC&&code!=ORIENTATION)return super.onTransact(code,data,reply,flags);data.enforceInterface(ROOT);
            if(code==VSYNC){var callback=Objects.requireNonNull(data.readStrongBinder());data.enforceNoDataAvail();schedule(callback);return true;}
            if(code==ORIENTATION){boolean landscape=data.readBoolean();data.enforceNoDataAvail();
                // Same original display-mode operation as the native window
                // owner; load the matching native API only when it is used.
                System.loadLibrary("android_servers");var ids=com.android.server.display.DisplayControl.getPhysicalDisplayIds();if(ids.length==0)throw new IllegalStateException("Native display has no physical identity");var token=com.android.server.display.DisplayControl.getPhysicalDisplayToken(ids[0]);var display=SurfaceControl.getDynamicDisplayInfo(ids[0]);var mode=Arrays.stream(display.supportedDisplayModes).filter(value->(value.width>=value.height)==landscape).findFirst().orElseThrow(()->new IllegalStateException("Native display orientation mode is unavailable"));var specs=SurfaceControl.getDesiredDisplayModeSpecs(token);specs.defaultMode=mode.id;specs.allowGroupSwitching=true;if(!SurfaceControl.setDesiredDisplayModeSpecs(token,specs))throw new IllegalStateException("Native display mode did not commit");var bounds=new Rect(0,0,mode.width,mode.height);try(var transaction=new SurfaceControl.Transaction()){transaction.setDisplayProjection(token,0,bounds,bounds).apply(true);}reply.writeNoException();return true;
            }
            if(code==CONFIGURE){int count=data.readInt();if(count<0)throw new IllegalArgumentException("Invalid native surface transaction length");
                try(var transaction=new SurfaceControl.Transaction()){for(int i=0;i<count;i++){
                    var selected=Objects.requireNonNull(layer(data.readStrongBinder()));var next=layer(data.readStrongBinder());var relative=layer(data.readStrongBinder());var control=selected.control;
                    int stateFlags=data.readInt();float x=data.readFloat(),y=data.readFloat();int z=data.readInt();float alpha=data.readFloat(),dsdx=data.readFloat(),dtdx=data.readFloat(),dsdy=data.readFloat(),dtdy=data.readFloat();var crop=data.readTypedObject(Rect.CREATOR);var destination=data.readTypedObject(Rect.CREATOR);float radius=data.readFloat();Integer color=data.readBoolean()?data.readInt():null;
                    transaction.reparent(control,next==null?parent:next.control).setPosition(control,x,y).setMatrix(control,dsdx,dtdx,dsdy,dtdy).setAlpha(control,alpha).setVisibility(control,(stateFlags&1)==0).setWindowCrop(control,crop).setCornerRadius(control,radius);
                    if(relative==null)transaction.setLayer(control,z);else transaction.setRelativeLayer(control,relative.control,z);
                    if(destination!=null)transaction.setDestinationFrame(control,destination);else transaction.setDestinationFrame(control,new Rect(0,0,selected.width,selected.height));
                    if(color==null)transaction.unsetColor(control);else transaction.setColor(control,new float[]{Color.red(color)/255f,Color.green(color)/255f,Color.blue(color)/255f});
                }
                var completed=Objects.requireNonNull(layer(data.readStrongBinder()));long frame=data.readLong();data.enforceNoDataAvail();transaction.apply();barrierNative(root,completed,frame);reply.writeNoException();return true;}
            }
            String name=Objects.requireNonNull(data.readString());int width=data.readInt(),height=data.readInt(),creationFlags=data.readInt();var ancestor=layer(data.readStrongBinder());IBinder lifetime=Objects.requireNonNull(data.readStrongBinder());data.enforceNoDataAvail();
            if(root==0)throw new IllegalStateException("Native composition is closed");dimensions(width,height);
            var builder=new SurfaceControl.Builder().setName(name).setParent(ancestor==null?parent:ancestor.control);if((creationFlags&0xf0000)!=0x80000&&(creationFlags&0xf0000)!=0x20000)builder.setBufferSize(width,height);builder.setFlags(creationFlags);
            var control=builder.build();var layer=new ServerLayer(this,control,width,height,lifetime);
            try{lifetime.linkToDeath(layer,0);layers.add(layer);reply.writeNoException();reply.writeStrongBinder(layer);}
            catch(Throwable failure){layer.close();throw failure;}
            return true;
        }
        private synchronized void retired(ServerLayer layer){layers.remove(layer);}
        @Override public void close(){long previous;List<ServerLayer> pending;synchronized(this){if(root==0)return;previous=root;root=0;pending=List.copyOf(layers);for(var value:clocks.values())try{value.callback.unlinkToDeath(value,0);}catch(NoSuchElementException ignored){}clocks.clear();if(clockHandler!=null)clockHandler.post(()->{if(receiver!=null){receiver.dispose();receiver=null;}clockThread.quitSafely();});}for(var layer:pending)layer.close();releaseControlNative(previous,false);}
    }
    private static final class Slot {
        final long id,buffer;final int[] description;
        ParcelFileDescriptor fence;
        boolean exported,dequeued,inFlight,retired;
        Slot(long id,int width,int height){this.id=id;buffer=bufferNative(width,height);try{description=descriptionNative(buffer);}catch(Throwable failure){releaseBufferNative(buffer);throw failure;}}
        // Retirement drops a reference, never reuses the pixels. The original
        // native consumer and its Metal import retain their own storage aliases
        // through completion; there is no Java waiter per abandoned buffer.
        void close(){NativeComposition.close(fence);fence=null;releaseBufferNative(buffer);}
    }
    private static final class ServerLayer extends Binder implements IBinder.DeathRecipient,AutoCloseable {
        private final Server owner;private final IBinder lifetime;private final SurfaceControl control;
        private long layer,nextId=1;
        private int width,height;
        private final Map<Long,Slot> slots=new LinkedHashMap<>();
        ServerLayer(Server owner,SurfaceControl control,int width,int height,IBinder lifetime){this.owner=owner;this.control=control;this.layer=rootNative(control);this.width=width;this.height=height;this.lifetime=lifetime;attachInterface(()->this,LAYER);}
        private void check(){if(layer==0)throw new IllegalStateException("Native surface layer is closed");}
        private Slot dequeued(long id){Slot slot=slots.get(id);if(slot==null||slot.retired||!slot.dequeued)throw new IllegalStateException("Native surface buffer is not dequeued");return slot;}
        @Override protected synchronized boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            if(code<DEQUEUE||code>CLOSE)return super.onTransact(code,data,reply,flags);data.enforceInterface(LAYER);check();
            if(code==DEQUEUE){data.enforceNoDataAvail();Slot slot=null;for(var value:slots.values())if(!value.retired&&!value.dequeued&&!value.inFlight){slot=value;break;}
                long current=slots.values().stream().filter(value->!value.retired).count();
                if(slot==null&&current<3){slot=new Slot(nextId++,width,height);slots.put(slot.id,slot);}
                reply.writeNoException();reply.writeBoolean(slot!=null);if(slot==null)return true;
                slot.dequeued=true;reply.writeLong(slot.id);reply.writeBoolean(!slot.exported);
                if(!slot.exported){try(var storage=ParcelFileDescriptor.adoptFd(storageNative(slot.buffer))){reply.writeIntArray(slot.description);reply.writeTypedObject(storage,0);slot.exported=true;}catch(IOException failure){slot.dequeued=false;throw new IllegalStateException(failure);}catch(RuntimeException|Error failure){slot.dequeued=false;throw failure;}}
                reply.writeTypedObject(slot.fence,0);NativeComposition.close(slot.fence);slot.fence=null;return true;
            }
            if(code==QUEUE){long id=data.readLong(),frame=data.readLong();var fence=data.readTypedObject(ParcelFileDescriptor.CREATOR);data.enforceNoDataAvail();Slot slot;
                try{slot=dequeued(id);}catch(Throwable failure){NativeComposition.close(fence);throw failure;}
                slot.dequeued=false;slot.inFlight=true;int descriptor=fence==null?-1:fence.detachFd();
                try{submitNative(layer,slot.buffer,descriptor,this,id,frame);}catch(Throwable failure){slot.inFlight=false;throw failure;}
            }else if(code==CANCEL){long id=data.readLong();data.enforceNoDataAvail();dequeued(id).dequeued=false;notifyClient(AVAILABLE);
            }else if(code==STATE){float x=data.readFloat(),y=data.readFloat();int z=data.readInt();float alpha=data.readFloat();boolean visible=data.readBoolean();data.enforceNoDataAvail();
                if(!Float.isFinite(x)||!Float.isFinite(y)||!Float.isFinite(alpha)||alpha<0||alpha>1)throw new IllegalArgumentException("Invalid native surface state");stateNative(layer,x,y,z,alpha,visible);
            }else if(code==RESIZE){int w=data.readInt(),h=data.readInt();data.enforceNoDataAvail();dimensions(w,h);if(w!=width||h!=height){width=w;height=h;for(var slot:List.copyOf(slots.values())){slot.retired=true;if(!slot.inFlight){slots.remove(slot.id);slot.close();}}}
            }else{data.enforceNoDataAvail();close();}
            reply.writeNoException();return true;
        }
        // The NDK callback transfers this original release-fence descriptor.
        // It may fire on any Binder thread, including before submit returns.
        private void notifyClient(int code){var request=Parcel.obtain();try{request.writeInterfaceToken(LAYER);lifetime.transact(code,request,null,IBinder.FLAG_ONEWAY);}catch(RemoteException gone){close();}finally{request.recycle();}}
        @SuppressWarnings("unused") private void released(long id,int descriptor){synchronized(this){
            var fence=descriptor<0?null:ParcelFileDescriptor.adoptFd(descriptor);Slot slot=slots.get(id);
            if(slot==null){NativeComposition.close(fence);throw new IllegalStateException("Native surface buffer has no release owner");}
            slot.inFlight=false;slot.fence=fence;if(slot.retired){slots.remove(id);slot.close();}}
            notifyClient(AVAILABLE);
        }
        @SuppressWarnings("unused") private void latched(long frame,long time){var data=Parcel.obtain();try{data.writeInterfaceToken(LAYER);data.writeLong(frame);data.writeLong(time);lifetime.transact(OPEN,data,null,IBinder.FLAG_ONEWAY);}catch(RemoteException gone){close();}finally{data.recycle();}}
        @Override public void binderDied(){close();}
        @Override public void close(){synchronized(this){if(layer==0)return;releaseControlNative(layer,true);layer=0;control.release();
            for(var slot:List.copyOf(slots.values())){slot.retired=true;if(!slot.inFlight){slots.remove(slot.id);slot.close();}}
            try{lifetime.unlinkToDeath(this,0);}catch(NoSuchElementException ignored){/* The original RPC death owner already retired the registration. */}}
            owner.retired(this);notifyClient(RETIRED);
        }
    }

    /** A retained independent native layer; a per-Surface Canvas target can use
     * its own producer without flattening another layer into its image. */
    public static final class Layer implements HostSurface.Target,AutoCloseable {
        private final IBinder remote;
        private final ReentrantLock dequeueLock=new ReentrantLock();
        private final IBinder.DeathRecipient death=()->retire(false);
        private long availability;
        private final Map<Long,List<java.util.function.LongConsumer>> waiting=new TreeMap<>();
        private final Binder lifetime=new Binder(){@Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            if(code==AVAILABLE){data.enforceInterface(LAYER);data.enforceNoDataAvail();synchronized(Layer.this){availability++;Layer.this.notifyAll();}return true;}
            if(code==RETIRED){data.enforceInterface(LAYER);data.enforceNoDataAvail();retire(false);return true;}
            if(code!=OPEN)return super.onTransact(code,data,reply,flags);data.enforceInterface(LAYER);long id=data.readLong(),time=data.readLong();data.enforceNoDataAvail();
            var ready=new ArrayList<java.util.function.LongConsumer>();long completed;
            synchronized(Layer.this){if(closed)return true;if(id>latchedFrame){latchedFrame=id;latch=time;}completed=latch;for(var entry:List.copyOf(waiting.entrySet()))if(entry.getKey()<=latchedFrame){ready.addAll(entry.getValue());waiting.remove(entry.getKey());}}
            for(var completion:ready)completion.accept(completed);return true;
        }};
        private final Map<Long,Storage> buffers=new HashMap<>();
        private final Map<Long,Buffer> acquired=new HashMap<>();
        private int width,height;private volatile boolean closed;
        private long frame,latchedFrame,latch;
        public Layer(IBinder server,String name,int width,int height){this(server,name,width,height,0,null);}
        public Layer(IBinder server,String name,int width,int height,int flags,Layer parent){
            dimensions(width,height);this.width=width;this.height=height;
            var request=Parcel.obtain();var reply=Parcel.obtain();
            try{request.writeInterfaceToken(ROOT);request.writeString(Objects.requireNonNull(name));request.writeInt(width);request.writeInt(height);request.writeInt(flags);request.writeStrongBinder(parent==null?null:parent.remote);request.writeStrongBinder(lifetime);
                if(!server.transact(OPEN,request,reply,0))throw new IllegalStateException("Native composition endpoint is unavailable");reply.readException();remote=Objects.requireNonNull(reply.readStrongBinder());remote.linkToDeath(death,0);
            }catch(RemoteException failure){throw failure.rethrowAsRuntimeException();}finally{reply.recycle();request.recycle();}
        }
        private void check(){if(closed)throw new IllegalStateException("Native surface producer is closed");}
        /** A complete AOSP Surface uses this independent producer directly,
         * rather than flattening its pixels through a second scene compositor. */
        public synchronized android.view.Surface surface(){check();var result=new android.view.Surface();HostSurface.bind(result,width,height,1,this);return result;}
        @Override public long createNativeSurface(int width,int height,int format){return NativeSurface.createComposition(width,height,format,this);}
        /** Reserve before asynchronous GPU completion so frame listeners refer
         * to the queued producer operation, not a guessed host timestamp. */
        public synchronized long reserveFrame(){check();return ++frame;}
        private interface Request {void write(Parcel request);}
        private void transaction(int code,Request writer){var request=Parcel.obtain();var reply=Parcel.obtain();try{request.writeInterfaceToken(LAYER);writer.write(request);if(!remote.transact(code,request,reply,0))throw new IllegalStateException("Native surface endpoint is unavailable");reply.readException();}
            catch(RemoteException failure){throw failure.rethrowAsRuntimeException();}finally{reply.recycle();request.recycle();}}
        public void state(float x,float y,int z,float alpha,boolean visible){check();transaction(STATE,request->{request.writeFloat(x);request.writeFloat(y);request.writeInt(z);request.writeFloat(alpha);request.writeBoolean(visible);});}
        private static final class Storage implements AutoCloseable {
            final int width,height,stride;final ByteBuffer pixels;final ParcelFileDescriptor descriptor;final FileChannel channel;
            private int references=1;
            Storage(int[] description,ParcelFileDescriptor descriptor){
                if(description==null||description.length!=4||description[3]!=1||description[2]<description[0]){NativeComposition.close(descriptor);throw new IllegalArgumentException("Native buffer format is unsupported");}
                width=description[0];height=description[1];stride=description[2];
                this.descriptor=Objects.requireNonNull(descriptor);FileChannel opened=null;
                try{dimensions(width,height);long size=(long)stride*height*4;
                    opened=HostNio.newFileChannel(descriptor,descriptor.getFileDescriptor(),OsConstants.O_RDWR);
                    if(size>Integer.MAX_VALUE||opened.size()<size)throw new IllegalArgumentException("Native buffer storage is incomplete");
                    long page=Os.sysconf(OsConstants._SC_PAGESIZE),mapped=(size+page-1)/page*page;
                    // The physical allocator pads its coherent mapping to host
                    // pages. Metal borrows this same map without a pixel copy.
                    if(opened.size()<mapped)throw new IllegalArgumentException("Native buffer page padding is incomplete");
                    pixels=opened.map(FileChannel.MapMode.READ_WRITE,0,mapped);channel=opened;
                }catch(Throwable failure){if(opened!=null)try{opened.close();}catch(IOException closing){failure.addSuppressed(closing);}else NativeComposition.close(descriptor);throw failure instanceof RuntimeException?(RuntimeException)failure:new IllegalStateException("Native buffer storage cannot be mapped",failure);}
            }
            void write(int[] source){for(int y=0;y<height;y++)for(int x=0;x<width;x++){int value=source[y*width+x],offset=(y*stride+x)*4;pixels.put(offset,(byte)(value>>>16));pixels.put(offset+1,(byte)(value>>>8));pixels.put(offset+2,(byte)value);pixels.put(offset+3,(byte)(value>>>24));}}
            synchronized void retain(){if(references==0)throw new IllegalStateException("Native buffer storage is retired");references++;}
            @Override public synchronized void close(){if(--references!=0)return;HostNio.freeDirectBuffer(pixels);try{channel.close();}catch(IOException failure){throw new IllegalStateException(failure);}}
        }
        /** One dequeue from the same bounded native owner. GPU producers take
         * the acquire fence and retain their duplicated storage FD until their
         * own commands finish, then queue the real completion fence. */
        public final class Buffer implements AutoCloseable {
            private final long id;private final Storage storage;private ParcelFileDescriptor fence;private boolean done;private Cancellation cancellation;
            private Buffer(long id,Storage storage,ParcelFileDescriptor fence){this.id=id;this.storage=storage;this.fence=fence;storage.retain();}
            private void active(){check();if(done||acquired.get(id)!=this)throw new IllegalStateException("Native buffer dequeue has ended");}
            public int width(){return storage.width;}public int height(){return storage.height;}public int stride(){return storage.stride;}
            public ParcelFileDescriptor duplicateStorage()throws IOException{synchronized(Layer.this){active();return ParcelFileDescriptor.dup(storage.descriptor.getFileDescriptor());}}
            public ParcelFileDescriptor takeAcquireFence(){synchronized(Layer.this){active();var value=fence;fence=null;return value;}}
            public ByteBuffer pixels(){ParcelFileDescriptor waiting;Cancellation cancel;
                synchronized(Layer.this){active();waiting=fence;fence=null;if(waiting!=null&&cancellation==null)cancellation=new Cancellation();cancel=cancellation;}
                awaitFence(waiting,cancel);
                synchronized(Layer.this){active();return storage.pixels.duplicate();}
            }
            /** Takes ownership of the producer's real completion fence. */
            public void queue(ParcelFileDescriptor completionFence,long frameId){
                try{
                    synchronized(Layer.this){if(closed){close();return;}active();if(fence!=null)throw new IllegalStateException("Native buffer acquire fence was not consumed");done=true;acquired.remove(id);frame=Math.max(frame,frameId);}
                    try{transaction(QUEUE,call->{call.writeLong(id);call.writeLong(frameId);call.writeTypedObject(completionFence,0);});}finally{if(cancellation!=null)cancellation.close();storage.close();}
                }finally{NativeComposition.close(completionFence);}
            }
            @Override public void close(){ParcelFileDescriptor previous;boolean notify;
                synchronized(Layer.this){if(done)return;done=true;acquired.remove(id);previous=fence;fence=null;notify=!closed;}
                try{if(cancellation!=null)cancellation.close();NativeComposition.close(previous);if(notify)transaction(CANCEL,call->call.writeLong(id));}finally{storage.close();}
            }
            private void cancelWait(){synchronized(Layer.this){if(cancellation!=null)cancellation.close();}}
        }
        public Buffer dequeue(){
            dequeueLock.lock();
            var request=Parcel.obtain();var reply=Parcel.obtain();long id=0;boolean received=false;Storage transferred=null;ParcelFileDescriptor fence=null;
            try{check();request.writeInterfaceToken(LAYER);if(!remote.transact(DEQUEUE,request,reply,0))throw new IllegalStateException("Native surface endpoint is unavailable");reply.readException();if(!reply.readBoolean())return null;
                id=reply.readLong();if(reply.readBoolean())transferred=new Storage(reply.createIntArray(),reply.readTypedObject(ParcelFileDescriptor.CREATOR));
                fence=reply.readTypedObject(ParcelFileDescriptor.CREATOR);
                synchronized(this){check();if(transferred!=null){if(buffers.putIfAbsent(id,transferred)!=null)throw new IllegalStateException("Native buffer identity was reused");transferred=null;}
                    var result=new Buffer(id,Objects.requireNonNull(buffers.get(id)),fence);fence=null;acquired.put(id,result);received=true;return result;}
            }catch(RemoteException failure){throw failure.rethrowAsRuntimeException();}
            finally{reply.recycle();request.recycle();if(transferred!=null)transferred.close();NativeComposition.close(fence);try{if(id!=0&&!received&&!closed){long selected=id;transaction(CANCEL,call->call.writeLong(selected));}}finally{dequeueLock.unlock();}}
        }
        /** Native GPU producers retain the ordinary blocking BufferQueue bound. */
        public Buffer dequeueBlocking(){for(;;){long generation;synchronized(this){check();generation=availability;}var value=dequeue();if(value!=null)return value;
            synchronized(this){check();while(availability==generation&&!closed)try{wait();}catch(InterruptedException interrupted){Thread.currentThread().interrupt();throw new IllegalStateException("Native buffer dequeue was interrupted",interrupted);}check();}
        }}
        @Override public boolean submit(int[] pixels,int w,int h,long frameId,long sourceTimeNanos){
            synchronized(this){check();if(w!=width||h!=height||pixels.length<Math.multiplyExact(w,h))throw new IllegalArgumentException("Native surface frame does not match its producer");}
            try(var selected=dequeue()){if(selected==null)return false;selected.pixels();selected.storage.write(pixels);selected.queue(null,frameId);return true;}
        }
        @Override public void onFrameCompleted(long id,java.util.function.LongConsumer completion){long time;synchronized(this){check();if(id>frame)throw new IllegalStateException("Native surface frame is not queued");if(id>latchedFrame){waiting.computeIfAbsent(id,unused->new ArrayList<>()).add(completion);return;}time=latch;}completion.accept(time);}
        @Override public void resize(int w,int h){dimensions(w,h);dequeueLock.lock();try{List<Storage> retired;
            synchronized(this){check();if(w==width&&h==height)return;if(!acquired.isEmpty())throw new IllegalStateException("Native buffer dequeues must finish before resize");}
            transaction(RESIZE,request->{request.writeInt(w);request.writeInt(h);});
            synchronized(this){check();retired=List.copyOf(buffers.values());buffers.clear();width=w;height=h;availability++;notifyAll();}for(var storage:retired)storage.close();
        }finally{dequeueLock.unlock();}}
        private void retire(boolean notify){List<Storage> previous;List<Buffer> pending;
            synchronized(this){if(closed)return;closed=true;waiting.clear();availability++;notifyAll();previous=List.copyOf(buffers.values());buffers.clear();pending=List.copyOf(acquired.values());}
            for(var buffer:pending)buffer.cancelWait();for(var storage:previous)storage.close();
            try{if(notify)transaction(CLOSE,request->{});}finally{try{remote.unlinkToDeath(death,0);}catch(NoSuchElementException ignored){}}
        }
        @Override public void close(){retire(true);}
    }
    private static native long rootNative(SurfaceControl parent);
    private static native long layerNative(long parent,String name);
    private static native void barrierNative(long root,Object completed,long frame);
    private static native void releaseControlNative(long pointer,boolean detach);
    private static native void stateNative(long layer,float x,float y,int z,float alpha,boolean visible);
    private static native long bufferNative(int width,int height);
    private static native int[] descriptionNative(long buffer);
    private static native int storageNative(long buffer);
    private static native void releaseBufferNative(long buffer);
    private static native void submitNative(long layer,long buffer,int acquireFence,Object callback,long token,long frame);
}
