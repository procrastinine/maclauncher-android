package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.content.pm.ParceledListSlice;
import android.os.*;
import java.lang.reflect.*;
import java.util.*;

/** Private channel storage and disabled delivery behind the original AOSP notification client. */
public final class HostNotificationService {
 private HostNotificationService(){}
 private static HostNotificationChannels channels;
 private static String packageName;
 private static boolean registered;
 private static final Binder binder=new Binder();
 private static final INotificationManager service=(INotificationManager)Proxy.newProxyInstance(INotificationManager.class.getClassLoader(),new Class<?>[]{INotificationManager.class},HostNotificationService::invoke);
 static{binder.attachInterface(service,"android.app.INotificationManager");}
 public static synchronized void install(Context context){
  if(channels==null){packageName=context.getPackageName();channels=new HostNotificationChannels(context.createCredentialProtectedStorageContext().getFilesDir().toPath().resolve(".maclauncher-platform/notification-channels.bin"));}
  if(!Objects.equals(packageName,context.getPackageName()))throw new SecurityException("Other application notifications are unavailable");
  if(!registered){HostServices.register(Context.NOTIFICATION_SERVICE,service,"android.app.INotificationManager");registered=true;}
 }
 private static Object invoke(Object proxy,Method method,Object[] arguments)throws Throwable{
  Object[] a=arguments==null?new Object[0]:arguments;String operation=method.getName();
  if(method.getDeclaringClass()==Object.class)return switch(operation){case "toString"->"Local Android notification service";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==a[0];default->throw new AssertionError(operation);};
  if(operation.equals("asBinder"))return binder;
  if(a.length>0&&a[0] instanceof String claimed&&!Objects.equals(claimed,packageName))throw new SecurityException("Other application notifications are unavailable");
  return switch(operation){
   case "areNotificationsEnabled","areNotificationsEnabledForPackage","isNotificationPolicyAccessGranted","isNotificationPolicyAccessGrantedForPackage","isInCall" -> false;
   case "getPackageImportance" -> NotificationManager.IMPORTANCE_NONE;
   // No notification was delivered under the disabled permission policy.
   case "cancelNotificationWithTag","cancelAllNotifications","enqueueNotificationWithTag" -> null;
   case "createNotificationChannels" -> {var list=((ParceledListSlice<?>)a[1]).getList();var requested=new ArrayList<NotificationChannel>();for(Object item:list)requested.add((NotificationChannel)item);channels.createNotificationChannels(requested);yield null;}
   case "getNotificationChannel" -> {if((int)a[1]!=UserHandle.myUserId()||!Objects.equals(packageName,a[2]))throw new SecurityException("Other application notifications are unavailable");yield channels.getNotificationChannel((String)a[3]);}
   case "getNotificationChannels" -> {if(!Objects.equals(packageName,a[1])||(int)a[2]!=UserHandle.myUserId())throw new SecurityException("Other application notifications are unavailable");yield new ParceledListSlice<>(channels.getNotificationChannels());}
   case "deleteNotificationChannel" -> {channels.deleteNotificationChannel((String)a[1]);yield null;}
   case "getActiveNotifications","getActiveNotificationsWithAttribution" -> new android.service.notification.StatusBarNotification[0];
   case "getAppActiveNotifications" -> new ParceledListSlice<>(List.of());
   default -> throw new RemoteException("Android notification operation is unavailable: "+operation);
  };
 }
}
