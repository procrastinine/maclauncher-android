package maclauncher.android.framework;

import android.content.*;
import android.os.*;
import java.util.*;

/** Application-private clipboard; the Mac pasteboard is never opened. */
public final class HostClipboardService extends IClipboard.Stub {
 private static final HostClipboardService INSTANCE=new HostClipboardService();
 private final RemoteCallbackList<IOnPrimaryClipChangedListener> listeners=new RemoteCallbackList<>();
 private ClipData clip;private String source,packageName;private int user;private Handler handler;private boolean pending,closed;
 private final Runnable deliver=()->{
  synchronized(this){pending=false;if(closed)return;int count=listeners.beginBroadcast();try{for(int i=0;i<count;i++)try{listeners.getBroadcastItem(i).dispatchPrimaryClipChanged();}catch(RemoteException gone){}}finally{listeners.finishBroadcast();}}
 };
 private HostClipboardService(){}
 public static synchronized void install(Context context){
  if(INSTANCE.handler==null){INSTANCE.packageName=context.getPackageName();INSTANCE.user=context.getUserId();INSTANCE.handler=new Handler(context.getMainLooper());HostServices.register(Context.CLIPBOARD_SERVICE,INSTANCE,"android.content.IClipboard");HostLooper.addShutdownListener(context.getMainLooper(),INSTANCE::close);}
 }
 private void check(String pkg,int id,int device){if(closed)throw new IllegalStateException("Clipboard owner has stopped");if(id!=user||device!=Context.DEVICE_ID_DEFAULT||!Objects.equals(packageName,pkg))throw new SecurityException("Other application clipboards are unavailable");}
 private static ClipData copy(ClipData value){if(value==null)return null;if(value.getItemCount()>128)throw new IllegalArgumentException("Too many clipboard items");Parcel parcel=Parcel.obtain();try{value.writeToParcel(parcel,0);if(parcel.dataSize()>1048576)throw new IllegalArgumentException("Clipboard data is too large");parcel.setDataPosition(0);return ClipData.CREATOR.createFromParcel(parcel);}finally{parcel.recycle();}}
 private void changed(){if(pending||listeners.getRegisteredCallbackCount()==0)return;pending=true;if(!handler.post(deliver)){pending=false;throw new IllegalStateException("Clipboard callback owner has stopped");}}
 private synchronized void close(){closed=true;handler.removeCallbacks(deliver);pending=false;listeners.kill();clip=null;source=null;}
 @Override public synchronized void setPrimaryClip(ClipData value,String pkg,String tag,int id,int device){check(pkg,id,device);clip=copy(Objects.requireNonNull(value));source=pkg;changed();}
 @Override public void setPrimaryClipAsPackage(ClipData value,String pkg,String tag,int id,int device,String target){if(!Objects.equals(pkg,target))throw new SecurityException("Clipboard source impersonation is unavailable");setPrimaryClip(value,pkg,tag,id,device);}
 @Override public synchronized void clearPrimaryClip(String pkg,String tag,int id,int device){check(pkg,id,device);if(clip!=null){clip=null;source=null;changed();}}
 @Override public synchronized ClipData getPrimaryClip(String pkg,String tag,int id,int device){check(pkg,id,device);return copy(clip);}
 @Override public synchronized ClipDescription getPrimaryClipDescription(String pkg,String tag,int id,int device){check(pkg,id,device);return clip==null?null:new ClipDescription(clip.getDescription());}
 @Override public synchronized boolean hasPrimaryClip(String pkg,String tag,int id,int device){check(pkg,id,device);return clip!=null&&clip.getItemCount()>0;}
 @Override public synchronized boolean hasClipboardText(String pkg,String tag,int id,int device){check(pkg,id,device);return clip!=null&&clip.getItemCount()>0&&clip.getItemAt(0).getText()!=null&&clip.getItemAt(0).getText().length()>0;}
 @Override public synchronized String getPrimaryClipSource(String pkg,String tag,int id,int device){check(pkg,id,device);return source;}
 @Override public synchronized void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,String pkg,String tag,int id,int device){check(pkg,id,device);if(listeners.getRegisteredCallbackCount()>=256)throw new IllegalStateException("Clipboard listener limit reached");listeners.register(Objects.requireNonNull(listener));}
 @Override public synchronized void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,String pkg,String tag,int id,int device){check(pkg,id,device);listeners.unregister(Objects.requireNonNull(listener));}
 @Override public boolean areClipboardAccessNotificationsEnabledForUser(int id){if(id!=user)throw new SecurityException("Other Android users are unavailable");return false;}
 @Override public void setClipboardAccessNotificationsEnabledForUser(boolean enabled,int id){if(id!=user||enabled)throw new SecurityException("System clipboard notifications are unavailable");}
}
