package maclauncher.android.framework;

import android.app.*;import android.content.*;import android.content.res.Configuration;import android.os.*;
import java.lang.reflect.*;import java.util.*;

/** Application window configuration; no car, television or projection device. */
public final class HostUiModeService {
 private HostUiModeService(){}
 private static Context context;private static boolean registered;
 private static final Binder binder=new Binder();
 private static final Set<IUiModeManagerCallback> callbacks=Collections.newSetFromMap(new IdentityHashMap<>());
 private static final IUiModeManager service=(IUiModeManager)Proxy.newProxyInstance(IUiModeManager.class.getClassLoader(),new Class<?>[]{IUiModeManager.class},(proxy,method,arguments)->{
  Object[] a=arguments==null?new Object[0]:arguments;String name=method.getName();
  if(method.getDeclaringClass()==Object.class)return switch(name){case "toString"->"Local Android UI mode service";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==a[0];default->throw new AssertionError(name);};
  return switch(name){
   case "asBinder" -> binder;
   case "getCurrentModeType" -> Configuration.UI_MODE_TYPE_NORMAL;
   case "getNightMode" -> (context.getResources().getConfiguration().uiMode&Configuration.UI_MODE_NIGHT_MASK)==Configuration.UI_MODE_NIGHT_YES?UiModeManager.MODE_NIGHT_YES:UiModeManager.MODE_NIGHT_NO;
   case "getContrast" -> 0f;
   case "getForceInvertState","getActiveProjectionTypes" -> 0;
   case "isUiModeLocked","isNightModeLocked" -> true;
   case "getProjectingPackages" -> List.of();
   case "requestProjection","releaseProjection" -> false;
   case "addCallback" -> {synchronized(callbacks){if(callbacks.size()>=64&&!callbacks.contains(a[0]))throw new IllegalStateException("UI mode callback limit reached");callbacks.add((IUiModeManagerCallback)a[0]);}yield null;}
   default -> throw new RemoteException("Android UI mode operation is unavailable: "+name);
  };
 });
 static{binder.attachInterface(service,"android.app.IUiModeManager");}
 public static synchronized void install(Context owner){
  if(context==null)context=Objects.requireNonNull(owner);
  if(!registered){HostServices.register(Context.UI_MODE_SERVICE,service,"android.app.IUiModeManager");HostLooper.addShutdownListener(owner.getMainLooper(),()->{synchronized(callbacks){callbacks.clear();}context=null;});registered=true;}

 }
}
