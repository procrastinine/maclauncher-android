package maclauncher.android.bridge;
import android.os.*;
import android.system.*;
import android.view.MotionEvent;
import java.nio.*;
import java.util.*;
import maclauncher.android.framework.*;
/** Physical input from the existing Mac presenter; Android owns delivery. */
public class PhysicalInput implements AutoCloseable {
  final ParcelFileDescriptor descriptor;
  final MessageQueue queue=Looper.myQueue();
  final ByteBuffer packet=ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN);
  boolean touch;float lastX,lastY;ByteBuffer text;boolean composing;
  public PhysicalInput(ParcelFileDescriptor descriptor,Runnable closeRequested)throws Exception{
   this.descriptor=Objects.requireNonNull(descriptor);var fd=descriptor.getFileDescriptor();Os.fcntlInt(fd,OsConstants.F_SETFL,Os.fcntlInt(fd,OsConstants.F_GETFL,0)|OsConstants.O_NONBLOCK);
   queue.addOnFileDescriptorEventListener(fd,MessageQueue.OnFileDescriptorEventListener.EVENT_INPUT,(source,events)->{
    try{
     for(;;){int count;
      try{count=Os.read(source,text==null?packet:text);}catch(ErrnoException error){if(error.errno==OsConstants.EAGAIN)return MessageQueue.OnFileDescriptorEventListener.EVENT_INPUT;if(error.errno==OsConstants.EINTR)continue;throw error;}
      if(count==0){focus(false);return 0;}
      if(text!=null){if(text.hasRemaining())continue;text.flip();int start=text.getInt(),length=text.getInt();String value=java.nio.charset.StandardCharsets.UTF_8.decode(text).toString();text=null;HostTextInput.text(value,composing,start,length);continue;}
      if(packet.hasRemaining())continue;
      packet.flip();int kind=packet.getInt(),action=packet.getInt(),code=packet.getInt(),meta=packet.getInt();float x=packet.getFloat(),y=packet.getFloat(),dx=packet.getFloat(),dy=packet.getFloat();packet.clear();
      if(kind==10){HostActivity.hostSleep(action!=0);continue;}
      switch(kind){
       case 1->focus(action!=0);
       case 2->{lastX=x;lastY=y;if(action==MotionEvent.ACTION_DOWN)touch=true;else if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)touch=false;HostWindowManager.input(HostInput.motion(action,x,y));}
       case 3->HostWindowManager.input(HostInput.key(action,code,0,meta));
       case 4->HostInput.wheel(x,y,dx,dy,true);
       case 5->{if(closeRequested!=null)closeRequested.run();return 0;}
       case 6->{if(code<8)throw new IllegalStateException("Invalid native text length");text=ByteBuffer.allocate(code).order(ByteOrder.LITTLE_ENDIAN);composing=action!=0;}
       case 7->HostTextInput.shortcut(code);
       case 9->{if(action<1||code<1)throw new IllegalStateException("Invalid committed native viewport");HostDisplay.hostPublish(Integer.toUnsignedLong(meta),action,code,x,y);HostWindowManager.resizeOutput(action,code);HostActivity.outputResized(action,code);}
       default->throw new IllegalStateException("Unknown physical input record "+kind);
      }
     }
    }catch(ErrnoException|java.io.InterruptedIOException error){throw new IllegalStateException("Physical input capability failed",error);}
   });
  }
  private void focus(boolean value){if(!value&&touch){HostWindowManager.input(HostInput.motion(MotionEvent.ACTION_CANCEL,lastX,lastY));touch=false;}HostWindowManager.focusOutput(value);}
  @Override public void close()throws Exception{queue.removeOnFileDescriptorEventListener(descriptor.getFileDescriptor());focus(false);descriptor.close();}
 }
