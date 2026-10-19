package maclauncher.android.location;

import android.content.Context;
import android.location.*;
import android.location.provider.ProviderProperties;
import android.os.*;
import java.lang.reflect.*;
import java.util.*;
import maclauncher.android.framework.HostLooper;
import maclauncher.android.framework.HostServices;

/** Manual fixes behind the original Android location client and callback transports. */
public final class HostLocationService {
    private HostLocationService() {}
    private static final List<String> PROVIDERS=List.of(LocationManager.FUSED_PROVIDER);
    private static final ProviderProperties PROPERTIES=new ProviderProperties.Builder()
        .setPowerUsage(ProviderProperties.POWER_USAGE_LOW).setAccuracy(ProviderProperties.ACCURACY_FINE).build();
    private static final Object lock=new Object();
    private static final Map<IBinder,Request> requests=new IdentityHashMap<>();
    private static Context context;
    private static Handler handler;
    private static boolean registered,closed;
    private static final Binder binder=new Binder();
    private static final ILocationManager service=(ILocationManager)Proxy.newProxyInstance(
        ILocationManager.class.getClassLoader(),new Class<?>[]{ILocationManager.class},HostLocationService::invoke);
    static {binder.attachInterface(service,"android.location.ILocationManager");}

    public static void install(Context owner) {
        synchronized(lock) {
            if(closed)throw new IllegalStateException("Location service is closed");
            if(context==null) {context=Objects.requireNonNull(owner);handler=new Handler(owner.getMainLooper());}
            if(!registered) {
                HostServices.register(Context.LOCATION_SERVICE,service,"android.location.ILocationManager");
                if(!HostLooper.addShutdownListener(handler.getLooper(),HostLocationService::close))throw new IllegalStateException("Location owner has stopped");
                registered=true;
            }
        }
    }
    private static void provider(String value) {if(!PROVIDERS.contains(Objects.requireNonNull(value)))throw new IllegalArgumentException("Unknown location provider: "+value);}
    private static void packageName(String value) {if(!context.getPackageName().equals(value))throw new SecurityException("Location request belongs to another application");}
    private static boolean enabled() {synchronized(lock){return !closed&&!ManualLocation.wasDenied();}}
    private static Location fix(Location value,String provider) {if(value==null)return null;Location copy=new Location(value);copy.setProvider(provider);return copy;}
    private static List<String> providers(Criteria criteria,boolean enabledOnly) {
        if(enabledOnly&&!enabled())return List.of();
        if(criteria==null)return PROVIDERS;
        return PROVIDERS.stream().filter(name->LocationProvider.propertiesMeetCriteria(name,PROPERTIES,criteria)).toList();
    }
    private static Object invoke(Object proxy,Method method,Object[] arguments)throws Throwable {
        Object[] a=arguments==null?new Object[0]:arguments;String name=method.getName();
        if(method.getDeclaringClass()==Object.class)return switch(name){case "toString"->"Manual Android location service";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==a[0];default->throw new AssertionError(name);};
        return switch(name) {
            case "asBinder" -> binder;
            case "hasProvider" -> PROVIDERS.contains(Objects.requireNonNull(a[0]));
            case "getAllProviders" -> new ArrayList<>(PROVIDERS);
            case "getProviders" -> new ArrayList<>(providers((Criteria)a[0],(boolean)a[1]));
            case "getBestProvider" -> {var values=providers((Criteria)a[0],(boolean)a[1]);yield values.isEmpty()?null:values.get(0);}
            case "getProviderProperties" -> {provider((String)a[0]);yield PROPERTIES;}
            case "isProviderEnabledForUser" -> PROVIDERS.contains(a[0])&&(int)a[1]==context.getUserId()&&enabled();
            case "isLocationEnabledForUser" -> (int)a[0]==context.getUserId()&&enabled();
            case "getLastLocation" -> {provider((String)a[0]);packageName((String)a[2]);yield ManualLocation.last((String)a[0]);}
            case "getCurrentLocation" -> {
                provider((String)a[0]);packageName((String)a[3]);
                var callback=(ILocationCallback)a[2];var request=new Request((String)a[0],(LocationRequest)a[1],callback,null);
                start(request);yield new ICancellationSignal.Stub(){@Override public void cancel(){request.retire();}};
            }
            case "registerLocationListener" -> {provider((String)a[0]);packageName((String)a[3]);start(new Request((String)a[0],(LocationRequest)a[1],null,(ILocationListener)a[2]));yield null;}
            case "unregisterLocationListener" -> {Request request;synchronized(lock){request=requests.get(((ILocationListener)a[0]).asBinder());}if(request!=null)request.retire();yield null;}
            case "requestListenerFlush" -> {provider((String)a[0]);var listener=(ILocationListener)a[1];synchronized(lock){if(!requests.containsKey(listener.asBinder()))throw new IllegalArgumentException("Location listener is not registered");}listener.onFlushComplete((int)a[2]);yield null;}
            case "isGeocodeAvailable","isProviderPackage","isExtraLocationControllerPackageEnabled","isAdasGnssLocationEnabledForUser","isAutomotiveGnssSuspended" -> false;
            case "getGnssCapabilities" -> GnssCapabilities.empty();
            case "getGnssYearOfHardware","getGnssBatchSize" -> 0;
            case "getGnssHardwareModelName","getGnssTimeMillis","getExtraLocationControllerPackage" -> null;
            case "getGnssAntennaInfos","getProviderPackages" -> List.of();
            case "getBackgroundThrottlingWhitelist" -> new String[0];
            default -> throw new RemoteException("Android location operation is unavailable: "+name);
        };
    }
    private static void start(Request request) {
        synchronized(lock) {
            if(closed)throw new IllegalStateException("Location service is closed");
            Request previous=requests.get(request.key);
            if(previous!=null)previous.retire();
            if(requests.size()>=64)throw new IllegalStateException("Location request limit reached");
            requests.put(request.key,request);
            long duration=Math.max(0,request.definition.getExpirationRealtimeMs(SystemClock.elapsedRealtime())-SystemClock.elapsedRealtime());
            if(duration!=Long.MAX_VALUE&&!handler.postDelayed(request.timeout,duration)){request.retire();throw new IllegalStateException("Location owner has stopped");}
            // The manual source supplies one genuine observation. It does not poll or
            // manufacture later timestamps for an unchanged coordinate.
            request.pending=ManualLocation.request(request::deliver);
            if(request.stopped)request.pending.close();
        }
    }
    private static final class Request {
        final String provider;final LocationRequest definition;final ILocationCallback callback;final ILocationListener listener;final IBinder key;
        final Runnable timeout=this::expire;
        ManualLocation.Request pending;boolean stopped;
        Request(String provider,LocationRequest definition,ILocationCallback callback,ILocationListener listener){this.provider=provider;this.definition=Objects.requireNonNull(definition);this.callback=callback;this.listener=listener;key=callback!=null?callback.asBinder():Objects.requireNonNull(listener).asBinder();}
        void expire(){if(callback!=null)deliver(null);else retire();}
        void deliver(Location value) {
            synchronized(lock) {
                if(stopped)return;
                if(callback!=null||value==null||definition.getMaxUpdates()==1)retire();
            }
            try {if(callback!=null)callback.onLocation(fix(value,provider));else if(value!=null)listener.onLocationChanged(List.of(fix(value,provider)),null);else listener.onProviderEnabledChanged(provider,false);}
            catch(RemoteException error){retire();}
            catch(RuntimeException|Error error){retire();throw error;}
        }
        void retire(){synchronized(lock){if(stopped)return;stopped=true;requests.remove(key,this);handler.removeCallbacks(timeout);if(pending!=null)pending.close();}}
    }
    private static void close(){synchronized(lock){closed=true;for(Request request:requests.values().toArray(Request[]::new))request.retire();}}
}
