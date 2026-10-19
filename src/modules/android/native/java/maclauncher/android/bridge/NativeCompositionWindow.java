package maclauncher.android.bridge;

import android.app.Activity;
import android.os.*;
import android.view.SurfaceControl;
import java.io.*;
import java.nio.*;
import java.util.Objects;
import maclauncher.android.framework.*;

/** JVM activity output over the supplied original native composition session.
 * The existing Mac presenter remains the only physical window/input/effects owner. */
public final class NativeCompositionWindow implements HostActivity.Window {
 private static final class End extends RuntimeException {final NativeCompositionWindow window;End(NativeCompositionWindow window){this.window=window;}}
 private final Thread owner=Thread.currentThread();
 private final NativeComposition.Session session;
 private final SurfaceControl root;
 private final PhysicalInput input;
 private final ParcelFileDescriptor control;
 private final OutputStream controls;
 private final AutoCloseable text;
 private int width,height;private boolean running,closed;
 public static void install(NativeComposition.Session session,int inputFd,int controlFd){
  Objects.requireNonNull(session);HostActivity.windowBinding((title,width,height,closeRequested,options)->new NativeCompositionWindow(session,inputFd,controlFd,closeRequested));
 }
 private NativeCompositionWindow(NativeComposition.Session session,int inputFd,int controlFd,Runnable closeRequested){
  this.session=session;width=session.width;height=session.height;control=ParcelFileDescriptor.adoptFd(controlFd);controls=new FileOutputStream(control.getFileDescriptor());
  root=HostSurfaceControl.nativeRootSurfaceControl("Android activity output",width,height,session);
  try{
   text=HostTextInput.physicalOutput(new HostTextInput.PhysicalOutput(){public void enabled(boolean enabled){command(enabled?1:0,0,0,0,0);}public void area(float left,float top,float right,float bottom){command(2,left,top,right,bottom);}});
   input=new PhysicalInput(ParcelFileDescriptor.adoptFd(inputFd),()->{if(closeRequested!=null)closeRequested.run();throw new End(this);});
   HostDisplay.hostPublish(session.screen,width,height,session.refresh,session.densityDpi/160f);session.startClock();
  }catch(Exception failure){root.release();try{control.close();}catch(IOException closing){failure.addSuppressed(closing);}session.close();throw new IllegalStateException("Native activity output could not attach",failure);}
 }
 private void check(){if(Thread.currentThread()!=owner||closed)throw new IllegalStateException("Native activity output requires its live owner thread");}
 private void command(int kind,float left,float top,float right,float bottom){check();byte[] bytes=ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN).putInt(kind).putFloat(left).putFloat(top).putFloat(right).putFloat(bottom).array();try{controls.write(bytes);}catch(IOException failure){throw new UncheckedIOException("Native text output failed",failure);}}
 @Override public void attachActivity(Activity activity){check();Objects.requireNonNull(activity);}
 @Override public void present(Activity activity,String title,int ignoredWidth,int ignoredHeight){
  check();HostWindowManager.attachOutput(activity,root,width,height,(current,landscape)->session.orientation(landscape));running=true;
  try{try{Looper.loop();}catch(End ended){if(ended.window!=this)throw ended;}}
  finally{running=false;HostWindowManager.detachOutput();}
 }
 @Override public void resized(int width,int height){check();HostSurfaceControl.resizeRoot(root,width,height);this.width=width;this.height=height;}
 @Override public void taskEmpty(){check();if(running)throw new End(this);}
 @Override public void stopCallbacks(Runnable tasks){check();session.stopClock();tasks.run();if(HostDisplayEvents.hostHasInFlightCallbacks())throw new IllegalStateException("Native display callbacks remain active");}
 @Override public void close(){if(closed)return;check();if(running||HostWindowManager.outputAttached())throw new IllegalStateException("Native activity output still has an attached producer");Throwable first=null;
  try{input.close();}catch(Throwable failure){first=failure;}try{text.close();}catch(Throwable failure){if(first==null)first=failure;else first.addSuppressed(failure);}try{control.close();}catch(Throwable failure){if(first==null)first=failure;else first.addSuppressed(failure);}try{root.release();}catch(Throwable failure){if(first==null)first=failure;else first.addSuppressed(failure);}try{session.close();}catch(Throwable failure){if(first==null)first=failure;else first.addSuppressed(failure);}closed=true;
  if(first instanceof RuntimeException failure)throw failure;if(first instanceof Error failure)throw failure;if(first!=null)throw new IllegalStateException("Native activity output did not retire",first);
 }
}
