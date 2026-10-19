package maclauncher.android.framework;

import android.graphics.*;
import android.os.Parcel;
import android.view.SurfaceControl;
import android.view.SurfaceSession;
import java.util.*;
import java.lang.ref.WeakReference;
import maclauncher.android.bridge.NativeSurface;
import maclauncher.android.bridge.NativeComposition;

/** Local compositor state beneath AOSP SurfaceControl/WindowlessWindowManager.
 * Native game buffers retain the existing IOSurface/ANativeWindow owner. */
public final class HostSurfaceControl {
    static final Object scene=new Object();
    private HostSurfaceControl(){}
    private static final class Layer implements AutoCloseable {
        private static final java.util.concurrent.atomic.AtomicInteger ids=new java.util.concurrent.atomic.AtomicInteger();
        final int id=ids.incrementAndGet();
        final String name;
        int width,height,format,flags,z,transformHint;
        long parent,buffer;
        float x,y,alpha=1,dsdx=1,dtdx,dsdy,dtdy=1;
        Rect crop,destination;
        float cornerRadius;
        Integer color;
        WeakReference<Layer> relative;
        Output output;
        Snapshot snapshot;
        final ArrayList<WeakReference<Layer>> children=new ArrayList<>();
        boolean closed,released;
        Layer(String name,int width,int height,int format,int flags){this.name=name;this.width=width;this.height=height;this.format=format;this.flags=flags;}
        @Override public void close(){synchronized(scene){
            if(released)return;released=true;
            if(buffer!=0){long previous=buffer;buffer=0;HostNativeHandle.release(previous);}else finish();
        }}
        void finish(){if(closed)return;var previousOutput=output(this);closed=true;if(parent!=0){long previous=parent;parent=0;layer(previous).children.removeIf(ref->ref.get()==null||ref.get()==this);HostNativeHandle.release(previous);}snapshot=null;if(previousOutput!=null)previousOutput.retired(this);if(output!=null){output.close();output=null;}}
    }
    private static final class Snapshot {
        int[] pixels;int width,height;
        // The producer updates its one array only under the scene lock; the
        // target consumes/copies it before returning. No extra full-frame copy.
        void set(int[] source,int width,int height){pixels=source;this.width=width;this.height=height;}
    }
    /** One bounded software composition target for Canvas windows. */
    private static class Output implements AutoCloseable {
        final HostSurface.Target target;final Layer root;
        Bitmap bitmap;Canvas canvas;int[] pixels;
        int width,height;long frame,nativeGeneration=NativeSurface.externalGeneration();
        final Paint paint=new Paint(Paint.FILTER_BITMAP_FLAG);
        final Paint nativeHole=new Paint();
        final Path rounding=new Path();
        final RectF roundBounds=new RectF();
        final Runnable repaint;
        final ArrayList<java.util.function.LongConsumer> completions=new ArrayList<>();
        Output(HostSurface.Target target,Layer root){nativeHole.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.DST_OUT));this.target=target;this.root=root;width=root.width;height=root.height;repaint=this::repaint;}
        private void repaint(){
            List<java.util.function.LongConsumer> ready;long submitted;
            synchronized(scene){if(root.closed)return;long previous=frame;if(!present(System.nanoTime())){HostDisplayEvents.requestPresentation(repaint);return;}submitted=frame==previous?0:frame;ready=List.copyOf(completions);completions.clear();}
            // Application executors can run inline. Never invoke them under the
            // scene lock, including for a synchronous offscreen target.
            for(var completion:ready)if(submitted==0)completion.accept(System.nanoTime());else target.onFrameCompleted(submitted,completion);
        }
        boolean submit(Layer changed,int[] source,int width,int height,long frame,long time){synchronized(scene){
            if(changed.snapshot==null)changed.snapshot=new Snapshot();changed.snapshot.set(source,width,height);
            return present(time);
        }}
        private List<Layer> children(Layer parent){var list=new ArrayList<Layer>();parent.children.removeIf(ref->ref.get()==null);for(var ref:parent.children){var child=ref.get();if(child!=null&&!child.closed)list.add(child);}return list;}
        private void collect(Layer parent,Set<Layer> all){if(!all.add(parent))return;for(var child:children(parent))collect(child,all);}
        private void ordered(Layer item,Map<Layer,List<Layer>> relatives,Set<Layer> relativeLayers,Set<Layer> visited,List<Layer> result){
            if(!visited.add(item))return;
            var around=new ArrayList<Layer>();for(var child:children(item))if(!relativeLayers.contains(child))around.add(child);
            around.addAll(relatives.getOrDefault(item,List.of()));around.sort(Comparator.<Layer>comparingInt(child->child.z).thenComparingInt(child->child.id));
            for(var child:around)if(child.z<0)ordered(child,relatives,relativeLayers,visited,result);
            result.add(item);
            for(var child:around)if(child.z>=0)ordered(child,relatives,relativeLayers,visited,result);
        }
        private List<Layer> drawOrder(){
            Set<Layer> all=Collections.newSetFromMap(new IdentityHashMap<>());collect(root,all);
            Map<Layer,List<Layer>> relatives=new IdentityHashMap<>();Set<Layer> relativeLayers=Collections.newSetFromMap(new IdentityHashMap<>());
            for(var item:all){var anchor=item.relative==null?null:item.relative.get();if(anchor!=null&&anchor!=item&&all.contains(anchor)){relatives.computeIfAbsent(anchor,unused->new ArrayList<>()).add(item);relativeLayers.add(item);}}
            var result=new ArrayList<Layer>();ordered(root,relatives,relativeLayers,Collections.newSetFromMap(new IdentityHashMap<>()),result);return result;
        }
        private static boolean nativeProducer(Layer item){return item.buffer!=0&&NativeSurface.externallyPresented(HostSurface.buffer(item.buffer).lease);}
        private void draw(Layer item){
            int save=canvas.save();
            try{
                // Relative Z changes draw order, never transform/visibility
                // ownership. SurfaceView commonly anchors below an ancestor
                // while retaining an intermediate Bounds container as parent.
                var ancestry=new ArrayList<Layer>();for(var at=item;;at=layer(at.parent)){ancestry.add(at);if(at==root||at.parent==0)break;}
                float alpha=1;
                for(int i=ancestry.size()-1;i>=0;i--){var at=ancestry.get(i);if(at.closed||(at.flags&1)!=0)return;
                    canvas.translate(at.x,at.y);var matrix=new Matrix();matrix.setValues(new float[]{at.dsdx,at.dtdx,0,at.dsdy,at.dtdy,0,0,0,1});canvas.concat(matrix);
                    if(at.crop!=null)canvas.clipRect(at.crop);
                    if(at.cornerRadius>0){
                        if(at.crop!=null)roundBounds.set(at.crop);else roundBounds.set(0,0,at.width>0?at.width:width,at.height>0?at.height:height);
                        rounding.reset();rounding.addRoundRect(roundBounds,at.cornerRadius,at.cornerRadius,Path.Direction.CW);canvas.clipPath(rounding);
                    }
                    alpha*=at.alpha;
                }
                if(item.color!=null){paint.setColor(item.color);paint.setAlpha(Math.round(Math.max(0,Math.min(1,alpha))*255));canvas.drawRect(0,0,item.width>0?item.width:width,item.height>0?item.height:height,paint);}
                if(nativeProducer(item)){
                    // The IOSurface is already presented below this software
                    // overlay. At its scene position it replaces lower layers,
                    // including SurfaceView's backing color; later UI stays above.
                    nativeHole.setAlpha(Math.round(Math.max(0,Math.min(1,alpha))*255));
                    if(item.destination!=null)canvas.drawRect(item.destination,nativeHole);
                    else canvas.drawRect(0,0,item.width,item.height,nativeHole);
                }else if(item.snapshot!=null){
                    paint.setAlpha(Math.round(Math.max(0,Math.min(1,alpha))*255));var image=item.snapshot;int bufferState=canvas.save();
                    try{if(item.destination!=null){canvas.translate(item.destination.left,item.destination.top);canvas.scale((float)item.destination.width()/image.width,(float)item.destination.height()/image.height);}canvas.drawBitmap(image.pixels,0,image.width,0,0,image.width,image.height,true,paint);}
                    finally{canvas.restoreToCount(bufferState);}
                }
            }finally{canvas.restoreToCount(save);}
        }
        boolean present(long time){
            var list=children(root);
            if(root.snapshot==null&&root.color==null&&list.stream().noneMatch(Output::hasImage))return true;
            // The ordinary single full-window Canvas keeps the original direct
            // transport without an extra Canvas composition pass.
            if(root.snapshot==null&&list.size()==1){var child=list.get(0);var image=child.snapshot;
                if(image!=null&&!nativeProducer(child)&&(child.flags&1)==0&&child.x==0&&child.y==0&&child.alpha==1&&child.dsdx==1&&child.dtdy==1&&child.dtdx==0&&child.dsdy==0&&child.crop==null&&child.cornerRadius==0&&(child.destination==null||child.destination.left==0&&child.destination.top==0&&child.destination.right==image.width&&child.destination.bottom==image.height)&&child.children.isEmpty()&&image.width==width&&image.height==height)
                    return target.submit(image.pixels,width,height,++frame,time);
            }
            if(bitmap==null){bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);canvas=new Canvas(bitmap);pixels=new int[Math.multiplyExact(width,height)];}
            bitmap.eraseColor(0);for(var item:drawOrder())draw(item);bitmap.getPixels(pixels,0,width,0,0,width,height);
            return target.submit(pixels,width,height,++frame,time);
        }
        private static boolean hasImage(Layer layer){if(layer.snapshot!=null||layer.color!=null||nativeProducer(layer))return true;for(var ref:layer.children){var child=ref.get();if(child!=null&&!child.closed&&hasImage(child))return true;}return false;}
        private void releaseImage(){if(canvas!=null)canvas.setBitmap(null);if(bitmap!=null)bitmap.recycle();canvas=null;bitmap=null;pixels=null;}
        void resize(int width,int height){if(this.width==width&&this.height==height)return;target.resize(width,height);releaseImage();this.width=width;this.height=height;}
        void retired(Layer layer){}
        @Override public void close(){HostDisplayEvents.cancelPresentation(repaint);completions.clear();releaseImage();}
    }
    /** Per-layer forwarding to the original native scene. This branch never
     * enters Output.draw/present's software composition or the Mac presenter. */
    private static final class NativeOutput extends Output {
        final NativeComposition.Session session;
        final Map<Layer,NativeComposition.Layer> layers=new IdentityHashMap<>();
        NativeOutput(NativeComposition.Session session,Layer root){super(new NativeComposition.Layer(session.root(),root.name,Math.max(1,root.width),Math.max(1,root.height),root.flags,null),root);this.session=session;layers.put(root,(NativeComposition.Layer)target);}
        NativeComposition.Layer producer(Layer owner){var value=layers.get(owner);if(value!=null)return value;
            if(owner.parent==0)throw new IllegalStateException("Surface has no native composition parent");
            value=new NativeComposition.Layer(session.root(),owner.name,Math.max(1,owner.width),Math.max(1,owner.height),owner.flags,producer(layer(owner.parent)));layers.put(owner,value);return value;
        }
        private void collect(Layer owner,List<Layer> output){if(owner.closed)return;output.add(owner);owner.children.removeIf(reference->reference.get()==null);for(var reference:owner.children){var value=reference.get();if(value!=null)collect(value,output);}}
        @Override boolean present(long time){var active=new ArrayList<Layer>();collect(root,active);var states=new ArrayList<NativeComposition.State>();
            Set<Layer> members=Collections.newSetFromMap(new IdentityHashMap<>());members.addAll(active);
            for(var owner:active){var relative=owner.relative==null?null:owner.relative.get();states.add(new NativeComposition.State(producer(owner),owner==root?null:producer(layer(owner.parent)),relative==null||!members.contains(relative)?null:producer(relative),owner.flags,owner.x,owner.y,owner.z,owner.alpha,owner.dsdx,owner.dtdx,owner.dsdy,owner.dtdy,owner.crop,owner.destination,owner.cornerRadius,owner.color));}
            frame=session.apply(states,(NativeComposition.Layer)target);return true;
        }
        @Override boolean submit(Layer changed,int[] pixels,int width,int height,long frame,long time){return producer(changed).submit(pixels,width,height,frame,time);}
        @Override void retired(Layer layer){var previous=layers.remove(layer);if(previous!=null)previous.close();}
        @Override public void close(){HostDisplayEvents.cancelPresentation(repaint);completions.clear();for(var value:List.copyOf(layers.values()))value.close();layers.clear();}
    }
    private static final class Transaction implements AutoCloseable {
        final ArrayList<Runnable> operations=new ArrayList<>();
        final ArrayList<Long> references=new ArrayList<>();
        final ArrayList<SurfaceControl.TransactionCommittedListener> committed=new ArrayList<>();
        final ArrayList<java.util.function.Consumer<SurfaceControl.TransactionStats>> completed=new ArrayList<>();
        void add(long handle,java.util.function.Consumer<Layer> operation){
            HostNativeHandle.retain(handle);references.add(handle);Layer target=layer(handle);operations.add(()->operation.accept(target));
        }
        @Override public void close(){operations.clear();committed.clear();completed.clear();for(long handle:references)HostNativeHandle.release(handle);references.clear();}
    }
    private static final class Completion implements java.util.function.LongConsumer {
        final List<java.util.function.Consumer<SurfaceControl.TransactionStats>> listeners;
        int pending;long latch;
        Completion(List<java.util.function.Consumer<SurfaceControl.TransactionStats>> listeners,int pending){this.listeners=listeners;this.pending=pending;}
        @Override public void accept(long time){
            synchronized(this){latch=Math.max(latch,time);if(--pending!=0)return;}
            for(var listener:listeners){
                // The original API explicitly permits an empty present fence
                // on devices without an exported Android fence descriptor.
                var stats=(SurfaceControl.TransactionStats)HostUiAccess.construct(SurfaceControl.TransactionStats.class,new Class<?>[]{long.class,long.class},latch,0L);
                listener.accept(stats);
            }
        }
    }
    private static Layer layer(long handle){return (Layer)HostNativeHandle.state(handle);}
    private static Transaction transaction(long handle){return (Transaction)HostNativeHandle.state(handle);}
    private static Output output(Layer layer){for(;;){if(layer.output!=null)return layer.output;if(layer.parent==0)return null;layer=layer(layer.parent);}}
    private static void parent(Layer child,long handle){
        if(handle==child.parent)return;
        Layer next=handle==0?null:layer(handle);
        for(Layer ancestor=next;ancestor!=null;ancestor=ancestor.parent==0?null:layer(ancestor.parent))if(ancestor==child)throw new IllegalArgumentException("Surface parent cycle");
        if(handle!=0)HostNativeHandle.retain(handle);
        if(child.parent!=0){long previous=child.parent;layer(previous).children.removeIf(ref->ref.get()==null||ref.get()==child);child.parent=0;HostNativeHandle.release(previous);}
        child.parent=handle;if(next!=null)next.children.add(new WeakReference<>(child));
    }
    public static SurfaceControl rootSurfaceControl(String name,int width,int height,HostSurface.Target target){
        var control=new SurfaceControl.Builder().setName(name).setBufferSize(width,height).setFormat(1).build();
        synchronized(scene){var root=layer(control.mNativeObject);root.flags&=~1;root.output=new Output(Objects.requireNonNull(target),root);}return control;
    }
    public static SurfaceControl nativeRootSurfaceControl(String name,int width,int height,NativeComposition.Session session){
        var control=new SurfaceControl.Builder().setName(name).setContainerLayer().setHidden(false).build();
        synchronized(scene){var root=layer(control.mNativeObject);root.width=width;root.height=height;root.output=new NativeOutput(Objects.requireNonNull(session),root);}return control;
    }
    /** Producer attachment changes invalidate composition once, not on every frame. */
    public static void refreshNativePresentation(SurfaceControl control){synchronized(scene){
        var root=layer(control.mNativeObject);var output=root.output;if(output==null)return;
        long generation=NativeSurface.externalGeneration();if(output.nativeGeneration==generation)return;
        output.nativeGeneration=generation;output.present(System.nanoTime());
    }}
    /** Called only for a committed change to the existing Mac window's extent. */
    public static void resizeRoot(SurfaceControl control,int width,int height){synchronized(scene){
        var root=layer(control.mNativeObject);if(root.output==null)throw new IllegalArgumentException("Surface is not a native window root");
        root.output.resize(width,height);root.width=width;root.height=height;
    }}
    static long bufferHandle(long control){synchronized(scene){
        var owner=layer(control);if(owner.buffer==0){
            int width=Math.max(1,owner.width),height=Math.max(1,owner.height),format=owner.format==2||owner.format==4?owner.format:1;
            owner.buffer=HostSurface.create(width,height,format,new HostSurface.Target(){
                public long createNativeSurface(int w,int h,int f){var root=output(owner);return root instanceof NativeOutput nativeOutput?nativeOutput.producer(owner).createNativeSurface(w,h,f):HostSurface.Target.super.createNativeSurface(w,h,f);}
                public boolean submit(int[] pixels,int width,int height,long frame,long time){var root=output(owner);if(root==null)throw new IllegalStateException("Surface has no window presentation owner");return root.submit(owner,pixels,width,height,frame,time);}
                public void resize(int width,int height){synchronized(scene){owner.width=width;owner.height=height;var root=output(owner);if(root instanceof NativeOutput nativeOutput)nativeOutput.producer(owner).resize(width,height);if(owner.output!=null)owner.output.resize(width,height);}}
                public void close(){synchronized(scene){if(owner.released)owner.finish();}}
            });
        }return owner.buffer;
    }}
    static long acquireBuffer(long control){return HostNativeHandle.retain(bufferHandle(control));}
    public static long nativeCreate(SurfaceSession session,String name,int width,int height,int format,int flags,long parent,Parcel metadata){
        var owner=new Layer(name,width,height,format,flags);synchronized(scene){try{parent(owner,parent);return HostNativeHandle.create(owner);}catch(Throwable failure){owner.close();throw failure;}}
    }
    public static long nativeCopyFromSurfaceControl(long handle){return HostNativeHandle.retain(handle);}
    public static long nativeGetNativeSurfaceControlFinalizer(){return HostNativeHandle.finalizer();}
    public static long nativeGetHandle(long handle){layer(handle);return handle;}
    public static int nativeGetLayerId(long handle){return layer(handle).id;}
    public static int nativeGetTransformHint(long handle){return layer(handle).transformHint;}
    public static void nativeSetTransformHint(long handle,int hint){if(handle==0)return;synchronized(scene){layer(handle).transformHint=hint;}}
    public static void nativeDisconnect(long handle){synchronized(scene){var owner=layer(handle);if(owner.buffer!=0){HostSurface.buffer(owner.buffer).close();HostNativeHandle.release(owner.buffer);owner.buffer=0;}}}
    public static void nativeUpdateDefaultBufferSize(long handle,int width,int height){synchronized(scene){var owner=layer(handle);if(owner.buffer!=0)HostSurface.buffer(owner.buffer).resize(width,height);owner.width=width;owner.height=height;}}
    public static long nativeCreateTransaction(){return HostNativeHandle.create(new Transaction());}
    public static long nativeGetNativeTransactionFinalizer(){return HostNativeHandle.finalizer();}
    public static long nativeGetTransactionId(long handle){transaction(handle);return handle;}
    public static void nativeSanitize(long handle,int pid,int uid){synchronized(scene){
        // A transaction owns only capabilities in this process-private scene.
        // It has no SurfaceFlinger global display/input commands to strip and
        // cannot acquire another application's layers through caller PID/UID.
        // Local transforms (including rotation) retain their ordinary meaning.
        transaction(handle);
    }}
    public static void nativeClearTransaction(long handle){synchronized(scene){transaction(handle).close();}}
    public static void nativeMergeTransaction(long handle,long other){synchronized(scene){if(handle==other)return;var target=transaction(handle);var source=transaction(other);target.operations.addAll(source.operations);target.references.addAll(source.references);target.committed.addAll(source.committed);target.completed.addAll(source.completed);source.operations.clear();source.references.clear();source.committed.clear();source.completed.clear();}}
    public static void nativeApplyTransaction(long handle,boolean sync,boolean oneWay){
        List<SurfaceControl.TransactionCommittedListener> callbacks;
        Completion immediate=null;
        synchronized(scene){var batch=transaction(handle);var outputs=Collections.newSetFromMap(new IdentityHashMap<Output,Boolean>());
            try{
                for(long reference:batch.references){var root=output(layer(reference));if(root!=null)outputs.add(root);}
                for(var operation:batch.operations)operation.run();
                for(long reference:batch.references){var root=output(layer(reference));if(root!=null)outputs.add(root);}
                callbacks=List.copyOf(batch.committed);
                if(!batch.completed.isEmpty()){
                    var completion=new Completion(List.copyOf(batch.completed),Math.max(1,outputs.size()));
                    if(outputs.isEmpty())immediate=completion;else for(var root:outputs)root.completions.add(completion);
                }
                for(var root:outputs)HostDisplayEvents.requestPresentation(root.repaint);
            }finally{batch.close();}}
        for(var callback:callbacks)callback.onTransactionCommitted();
        if(immediate!=null)immediate.accept(System.nanoTime());
    }
    private static void change(long transaction,long layer,java.util.function.Consumer<Layer> operation){synchronized(scene){transaction(transaction).add(layer,operation);}}
    public static void nativeSetLayer(long transaction,long layer,int z){change(transaction,layer,owner->owner.z=z);}
    public static void nativeSetRelativeLayer(long transaction,long layer,long relative,int z){change(transaction,layer,owner->{owner.relative=new WeakReference<>(layer(relative));owner.z=z;});}
    public static void nativeSetPosition(long transaction,long layer,float x,float y){change(transaction,layer,owner->{owner.x=x;owner.y=y;});}
    public static void nativeSetAlpha(long transaction,long layer,float alpha){change(transaction,layer,owner->owner.alpha=alpha);}
    public static void nativeSetColor(long transaction,long layer,float[] color){var copy=color.clone();change(transaction,layer,owner->owner.color=copy.length>=3&&copy[0]>=0&&copy[1]>=0&&copy[2]>=0?Color.rgb(copy[0],copy[1],copy[2]):null);}
    public static void nativeSetScale(long transaction,long layer,float x,float y){change(transaction,layer,owner->{owner.dsdx=x;owner.dtdy=y;});}
    public static void nativeSetMatrix(long transaction,long layer,float dsdx,float dtdx,float dsdy,float dtdy){change(transaction,layer,owner->{owner.dsdx=dsdx;owner.dtdx=dtdx;owner.dsdy=dsdy;owner.dtdy=dtdy;});}
    public static void nativeSetFlags(long transaction,long layer,int flags,int mask){change(transaction,layer,owner->owner.flags=(owner.flags&~mask)|(flags&mask));}
    public static void nativeSetWindowCrop(long transaction,long layer,int left,int top,int right,int bottom){change(transaction,layer,owner->owner.crop=right<=left||bottom<=top?null:new Rect(left,top,right,bottom));}
    public static void nativeSetCrop(long transaction,long layer,float left,float top,float right,float bottom){nativeSetWindowCrop(transaction,layer,(int)left,(int)top,(int)right,(int)bottom);}
    public static void nativeSetCornerRadius(long transaction,long layer,float radius){change(transaction,layer,owner->owner.cornerRadius=Math.max(0,radius));}
    public static void nativeSetDestinationFrame(long transaction,long layer,int left,int top,int right,int bottom){change(transaction,layer,owner->owner.destination=right<=left||bottom<=top?null:new Rect(left,top,right,bottom));}
    public static void nativeReparent(long transaction,long layer,long parent){if(parent!=0)synchronized(scene){HostNativeHandle.retain(parent);transaction(transaction).references.add(parent);}change(transaction,layer,owner->parent(owner,parent));}
    public static void nativeAddTransactionCommittedListener(long handle,SurfaceControl.TransactionCommittedListener listener){synchronized(scene){transaction(handle).committed.add(Objects.requireNonNull(listener));}}
    public static void nativeAddTransactionCompletedListener(long handle,java.util.function.Consumer<SurfaceControl.TransactionStats> listener){synchronized(scene){transaction(handle).completed.add(Objects.requireNonNull(listener));}}
    public static void nativeSetAnimationTransaction(long handle){transaction(handle);}
    public static void nativeSetEarlyWakeupStart(long handle){transaction(handle);}
    public static void nativeSetEarlyWakeupEnd(long handle){transaction(handle);}
    private static boolean displayHintReported;
    private static void displayHint(long handle,long surface){synchronized(scene){
        transaction(handle);if(surface!=0)layer(surface);
        // These are SurfaceFlinger display-mode/timeline preferences, not a
        // condition for presenting the transaction. The Mac window consumes
        // the actual CoreVideo display clock and does not control a separate
        // Android physical display or SurfaceFlinger frame timeline.
        if(!displayHintReported){displayHintReported=true;System.err.println("[android-host] Android display-mode and frame-timeline hints are unavailable; presentation uses the Mac display clock.");}
    }}
    public static void nativeSetFrameRateSelectionPriority(long transaction,long surface,int priority){displayHint(transaction,surface);}
    public static void nativeSetFrameRateSelectionStrategy(long transaction,long surface,int strategy){displayHint(transaction,surface);}
    public static void nativeSetFrameRateCategory(long transaction,long surface,int category,boolean smoothOnly){displayHint(transaction,surface);}
    public static void nativeSetDefaultFrameRateCompatibility(long transaction,long surface,int compatibility){displayHint(transaction,surface);}
    public static void nativeSetFrameRate(long transaction,long surface,float rate,int compatibility,int strategy){displayHint(transaction,surface);}
    public static void nativeSetFrameTimelineVsync(long transaction,long vsync){displayHint(transaction,0);}
    public static void nativeSetFrameTimeline(long transaction,long vsync){displayHint(transaction,0);}
    public static void nativeSetDesiredPresentTimeNanos(long transaction,long time){displayHint(transaction,0);}
    private static boolean hdrHintReported;
    private static void hdrHint(long handle,long surface,boolean requested){synchronized(scene){
        transaction(handle);layer(surface);
        // The current display capability and compositor buffers are SDR. AOSP
        // defines these as preferences, so requesting headroom cannot prevent
        // ordinary SurfaceView creation or change the SDR pixel interpretation.
        if(requested&&!hdrHintReported){hdrHintReported=true;System.err.println("[android-host] HDR headroom is not applied by the current SDR presentation path.");}
    }}
    public static void nativeSetDesiredHdrHeadroom(long transaction,long surface,float ratio){hdrHint(transaction,surface,ratio>1);}
    public static void nativeSetExtendedRangeBrightness(long transaction,long surface,float currentRatio,float desiredRatio){hdrHint(transaction,surface,currentRatio>1||desiredRatio>1);}
    public static void nativeSetDimmingEnabled(long transaction,long surface,boolean enabled){hdrHint(transaction,surface,false);}
    public static void nativeSetFixedTransformHint(long transaction,long layer,int hint){change(transaction,layer,owner->owner.transformHint=hint);}
    public static void nativeSetTransparentRegionHint(long transaction,long layer,Region region){change(transaction,layer,owner->{});/* Optional occlusion optimization; composition still uses real alpha. */}
    public static void nativeSetDamageRegion(long transaction,long layer,Region region){change(transaction,layer,owner->{});/* Full damage remains correct. */}
    public static void nativeAddWindowInfosReportedListener(long transaction,Runnable listener){synchronized(scene){transaction(transaction).committed.add(listener::run);}}
    public static boolean nativeGetProtectedContentSupport(){return false;}
    public static int nativeGetGPUContextPriority(){return 0;}
    public static int nativeGetMaxPictureProfiles(){return 0;}
    private static android.os.IBinder applyToken;
    public static void nativeSetDefaultApplyToken(android.os.IBinder token){synchronized(scene){applyToken=token;}}
    public static android.os.IBinder nativeGetDefaultApplyToken(){synchronized(scene){return applyToken;}}
}
