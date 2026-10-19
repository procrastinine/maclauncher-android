package maclauncher.android;

import android.app.Activity;
import android.os.*;
import android.view.*;
import java.util.*;
import java.util.concurrent.*;
import maclauncher.android.framework.*;

/** Original Android surfaces borrowing the application's existing physical output. */
final class ArtWindows {
 private static final String WINDOW="maclauncher.android.ArtWindow",INPUT="maclauncher.android.ArtWindowInput";
 private static final int OPEN=IBinder.FIRST_CALL_TRANSACTION,ORIENT=OPEN+1,CLOSE=OPEN+2;
 private static final int RESIZE=IBinder.FIRST_CALL_TRANSACTION,STATE=RESIZE+1,RETIRE=RESIZE+2,WHEEL=RESIZE+3,TEXT=RESIZE+4,SHORTCUT=RESIZE+5;
 private ArtWindows(){}
 static void install(SurfaceControl initial,boolean service){
  HostActivity.windowBinding((title,width,height,closeRequested,options)->new Window(initial,service,HostActivity.windowCapability(options),width,height,closeRequested));
 }
 private static final class EndWindow extends RuntimeException {final Window owner;EndWindow(Window owner){this.owner=owner;}}
 private static <T>T main(Callable<T> action)throws RemoteException{
  if(Looper.getMainLooper().isCurrentThread())try{return action.call();}catch(RuntimeException|Error failure){throw failure;}catch(Exception failure){throw new RemoteException(failure.toString());}
  CompletableFuture<T> result=new CompletableFuture<>();
  if(!new Handler(Looper.getMainLooper()).post(()->{try{result.complete(action.call());}catch(Throwable failure){result.completeExceptionally(failure);}}))throw new DeadObjectException();
  try{return result.get();}catch(InterruptedException failure){Thread.currentThread().interrupt();throw new RemoteException("Activity window request interrupted");}
  catch(ExecutionException failure){Throwable cause=failure.getCause();if(cause instanceof RuntimeException error)throw error;if(cause instanceof Error error)throw error;throw new RemoteException(cause.toString());}
 }
 private static final class Window implements HostActivity.Window {
  final Thread owner=Thread.currentThread();final Handler handler=new Handler(Looper.getMainLooper());
  final boolean service;final IBinder parent;final ArrayList<Child> children=new ArrayList<>();
  SurfaceControl surface;InputChannel channel;InputEventReceiver receiver;IBinder parentLease;Activity activity;int width,height;boolean closed,running,parentRetired,initialFocus=true;
  void retireFromParent(){if(closed||parentRetired)return;parentRetired=true;try{if(activity!=null)HostActivity.owner(activity).finish();}finally{taskEmpty();}}
  void applyState(boolean sleeping,boolean focused){
   // focusOutput deliberately ignores attempts to restore focus while asleep.
   // Apply the saved physical focus before sleep, or after waking, accordingly.
   initialFocus=focused;
   if(sleeping){HostWindowManager.focusOutput(focused);HostActivity.hostSleep(true);}
   else{HostActivity.hostSleep(false);HostWindowManager.focusOutput(focused);}
  }
  private final Object wheelLock=new Object();private boolean wheelPosted,wheelTouch;private float wheelX,wheelY,wheelDx,wheelDy;
  void wheel(float x,float y,float dx,float dy,boolean allowTouch){
   synchronized(wheelLock){wheelX=x;wheelY=y;wheelDx+=dx;wheelDy+=dy;wheelTouch=allowTouch;if(wheelPosted)return;wheelPosted=true;}
   handler.post(()->{float px,py,horizontal,vertical;boolean touch;synchronized(wheelLock){px=wheelX;py=wheelY;horizontal=wheelDx;vertical=wheelDy;touch=wheelTouch;wheelDx=wheelDy=0;wheelPosted=false;}if(!closed&&!parentRetired)HostInput.wheel(px,py,horizontal,vertical,touch);});
  }
  final IBinder.DeathRecipient parentDied=()->handler.post(this::retireFromParent);
  final Binder input=new Binder(){
   @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
    data.enforceInterface(INPUT);
    if(code==STATE){boolean sleeping=data.readBoolean(),focused=data.readBoolean();data.enforceNoDataAvail();handler.post(()->{if(!closed&&!parentRetired)applyState(sleeping,focused);});return true;}
    if(code==RETIRE){data.enforceNoDataAvail();handler.post(Window.this::retireFromParent);return true;}
    if(code==WHEEL){float x=data.readFloat(),y=data.readFloat(),dx=data.readFloat(),dy=data.readFloat();boolean allowTouch=data.readBoolean();data.enforceNoDataAvail();wheel(x,y,dx,dy,allowTouch);return true;}
    if(code==TEXT){String text=Objects.requireNonNull(data.readString());boolean composing=data.readBoolean();int start=data.readInt(),length=data.readInt();data.enforceNoDataAvail();handler.post(()->{if(!closed&&!parentRetired)HostTextInput.text(text,composing,start,length);});return true;}
    if(code==SHORTCUT){int key=data.readInt();data.enforceNoDataAvail();handler.post(()->{if(!closed&&!parentRetired)HostTextInput.shortcut(key);});return true;}
    if(code==RESIZE){long screen=data.readLong();int w=data.readInt(),h=data.readInt();float refresh=data.readFloat(),density=data.readFloat();data.enforceNoDataAvail();if(w<1||h<1)throw new IllegalArgumentException("Invalid activity output extent");handler.post(()->{if(!closed&&!parentRetired){HostDisplay.hostPublish(screen,w,h,refresh,density);HostWindowManager.resizeOutput(w,h);resized(w,h);}});return true;}
    return super.onTransact(code,data,reply,flags);
   }
  };
  final Binder capability=new Binder(){
   @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
    if(code==OPEN){data.enforceInterface(WINDOW);IBinder destination=Objects.requireNonNull(data.readStrongBinder());data.enforceNoDataAvail();main(()->{
     Child child=borrow(destination);try{reply.writeNoException();reply.writeTypedObject(child.surface,0);reply.writeStrongBinder(child);reply.writeTypedObject(child.channel,0);geometry(reply);reply.writeBoolean(HostActivity.hostSleeping());reply.writeBoolean(HostWindowManager.outputFocused());}
     catch(Throwable failure){child.close();throw failure;}finally{child.channel.dispose();}return null;
    });return true;}
    if(code==ORIENT){data.enforceInterface(WINDOW);boolean landscape=data.readBoolean();data.enforceNoDataAvail();main(()->{orientation(landscape);return null;});reply.writeNoException();return true;}
    return super.onTransact(code,data,reply,flags);
   }
  };
  final Runnable closeRequested;
  Window(SurfaceControl initial,boolean service,IBinder parent,int width,int height,Runnable closeRequested){
   this.closeRequested=closeRequested;
   this.service=service;this.parent=parent;this.width=width;this.height=height;
   if(parent==null){if(initial==null)throw new IllegalStateException("Activity has no delegated Android output");surface=initial;}
   else{
    Parcel data=Parcel.obtain(parent),reply=Parcel.obtain();
    try{data.writeInterfaceToken(WINDOW);data.writeStrongBinder(input);if(!parent.transact(OPEN,data,reply,0))throw new RemoteException("Android output delegation is unavailable");reply.readException();surface=reply.readTypedObject(SurfaceControl.CREATOR);parentLease=reply.readStrongBinder();channel=reply.readTypedObject(InputChannel.CREATOR);long screen=reply.readLong();this.width=reply.readInt();this.height=reply.readInt();float refresh=reply.readFloat(),density=reply.readFloat();if(surface==null||parentLease==null||channel==null||this.width<1||this.height<1)throw new IllegalStateException("Invalid delegated Android output");HostDisplay.hostPublish(screen,this.width,this.height,refresh,density);boolean sleeping=reply.readBoolean(),focused=reply.readBoolean();parentLease.linkToDeath(parentDied,0);applyState(sleeping,focused);
     receiver=new InputEventReceiver(channel,Looper.getMainLooper()){
      @Override public void onInputEvent(InputEvent event){if(closed||parentRetired){finishInputEvent(event,false);return;}HostWindowManager.input(event.copy(),handled->finishInputEvent(event,handled));}
     };}
    catch(RemoteException failure){try{close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure.rethrowAsRuntimeException();}
    catch(RuntimeException|Error failure){try{close();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}
    finally{data.recycle();reply.recycle();}
   }
  }
  void check(){if(Thread.currentThread()!=owner)throw new IllegalStateException("Activity output requires its owner thread");if(closed)throw new IllegalStateException("Activity output is closed");}
  void geometry(Parcel parcel){var metrics=new android.util.DisplayMetrics();HostDisplay.metrics(metrics);parcel.writeLong(HostDisplay.hostScreenId());parcel.writeInt(width);parcel.writeInt(height);parcel.writeFloat(HostDisplay.host().getRefreshRate());parcel.writeFloat(metrics.density);}
  Child borrow(IBinder destination)throws RemoteException{
   check();
   Child child=new Child(destination);try{destination.linkToDeath(child,0);children.add(child);try(var transaction=new SurfaceControl.Transaction()){transaction.setLayer(child.surface,children.size()).show(child.surface).apply();}updateInput();return child;}catch(Throwable failure){child.close();throw failure;}
  }
  void updateInput(){HostWindowManager.inputOutput(null);if(!children.isEmpty())HostWindowManager.inputOutput(children.getLast());hostState(HostActivity.hostSleeping(),HostWindowManager.outputFocused());}

  final class Child extends Binder implements IBinder.DeathRecipient,HostWindowManager.InputOutput {
   final IBinder destination;final SurfaceControl surface;final InputChannel channel,senderChannel;final InputEventSender sender;boolean retired;
   boolean send(int code,java.util.function.Consumer<Parcel> write){if(retired)return false;Parcel data=Parcel.obtain(destination);try{data.writeInterfaceToken(INPUT);write.accept(data);boolean accepted=destination.transact(code,data,null,IBinder.FLAG_ONEWAY);if(!accepted)binderDied();return accepted;}catch(RemoteException failure){binderDied();return false;}finally{data.recycle();}}
   Child(IBinder destination){this.destination=destination;
    InputChannel[] pair=InputChannel.openInputChannelPair("Android activity input");senderChannel=pair[0];channel=pair[1];SurfaceControl layer=null;
    try{layer=new SurfaceControl.Builder().setName("Android activity output").setContainerLayer().setParent(Window.this.surface).setHidden(false).build();
     sender=new InputEventSender(senderChannel,Looper.getMainLooper()){@Override public void onInputEventFinished(int sequence,boolean handled){HostWindowManager.inputFinished(Child.this,sequence,handled);}};surface=layer;
    }catch(Throwable failure){if(layer!=null)layer.release();senderChannel.dispose();channel.dispose();throw failure;}
   }
   @Override public boolean input(InputEvent event,java.util.function.Consumer<Boolean> callback){return !retired&&sender.sendInputEvent(event.getSequenceNumber(),event);}
   @Override public void wheel(float x,float y,float dx,float dy,boolean allowTouch){send(WHEEL,data->{data.writeFloat(x);data.writeFloat(y);data.writeFloat(dx);data.writeFloat(dy);data.writeBoolean(allowTouch);});}
   @Override public void text(String text,boolean composing,int start,int length){send(TEXT,data->{data.writeString(text);data.writeBoolean(composing);data.writeInt(start);data.writeInt(length);});}
   @Override public void shortcut(int code){send(SHORTCUT,data->data.writeInt(code));}
   void state(boolean sleeping,boolean focused){send(STATE,data->{data.writeBoolean(sleeping);data.writeBoolean(focused);});}
   void resize(){send(RESIZE,Window.this::geometry);}
   @Override public void binderDied(){handler.post(this::close);}
   @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{if(code!=CLOSE)return super.onTransact(code,data,reply,flags);data.enforceInterface(WINDOW);data.enforceNoDataAvail();main(()->{close();return null;});reply.writeNoException();return true;}
   void close(){if(retired)return;send(RETIRE,data->{});retired=true;children.remove(this);try{destination.unlinkToDeath(this,0);}catch(NoSuchElementException gone){}try(var transaction=new SurfaceControl.Transaction()){transaction.remove(surface).apply();}finally{surface.release();updateInput();sender.dispose();senderChannel.dispose();channel.dispose();}}
  }
  void orientation(boolean landscape){check();if(parent==null){ArtPlatform.orientation(landscape);return;}Parcel data=Parcel.obtain(parent),reply=Parcel.obtain();try{data.writeInterfaceToken(WINDOW);data.writeBoolean(landscape);if(!parent.transact(ORIENT,data,reply,0))throw new RemoteException("Android orientation delegation is unavailable");reply.readException();}catch(RemoteException failure){throw failure.rethrowAsRuntimeException();}finally{data.recycle();reply.recycle();}}
  @Override public IBinder capability(){return capability;}
  @Override public void attachActivity(Activity value){check();activity=Objects.requireNonNull(value);}
  @Override public void present(Activity value,String title,int ignoredWidth,int ignoredHeight){
   check();activity=value;HostActivity.resume(value);
   value.getWindow().getAttributes().inputFeatures|=WindowManager.LayoutParams.INPUT_FEATURE_NO_INPUT_CHANNEL;
   HostWindowManager.attachOutput(value,surface,width,height,(current,landscape)->orientation(landscape));
   HostWindowManager.focusOutput(initialFocus);running=true;
   try(var physical=service?null:new ArtActivity.PhysicalInput(ParcelFileDescriptor.adoptFd(6),closeRequested)){
    try{Looper.loop();}catch(EndWindow ended){if(ended.owner!=this)throw ended;}
   }catch(RuntimeException|Error failure){throw failure;}catch(Exception failure){throw new IllegalStateException("Android input capability could not close",failure);}
   finally{running=false;}
  }
  @Override public void hostState(boolean sleeping,boolean focused){check();for(Child child:List.copyOf(children))child.state(sleeping,focused&&child==children.getLast());}
  @Override public void resized(int width,int height){check();this.width=width;this.height=height;for(Child child:List.copyOf(children))child.resize();}
  @Override public void taskEmpty(){if(running)throw new EndWindow(this);}
  @Override public void stopCallbacks(Runnable ownerTasks){check();ownerTasks.run();}
  @Override public void close(){
   if(closed)return;check();if(running||HostWindowManager.outputAttached())throw new IllegalStateException("Activity output still has an attached producer");for(Child child:List.copyOf(children))child.close();HostWindowManager.inputOutput(null);
   if(parentLease!=null){try{parentLease.unlinkToDeath(parentDied,0);}catch(NoSuchElementException gone){}Parcel data=Parcel.obtain(parentLease),reply=Parcel.obtain();try{if(!parentRetired){data.writeInterfaceToken(WINDOW);if(parentLease.transact(CLOSE,data,reply,0))reply.readException();}}catch(RemoteException gone){}finally{data.recycle();reply.recycle();}parentLease=null;if(surface!=null)surface.release();}
   if(receiver!=null){receiver.dispose();receiver=null;}if(channel!=null){channel.dispose();channel=null;}surface=null;closed=true;
  }
 }
}
