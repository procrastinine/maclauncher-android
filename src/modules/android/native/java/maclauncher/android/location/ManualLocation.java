package maclauncher.android.location;

import android.location.Location;
import android.os.Handler;
import android.os.Looper;
import java.util.LinkedHashSet;
import java.util.function.Consumer;
import maclauncher.android.bridge.NativeLocation;

/** One application process, one explicit decision. There is no host location
 * lookup, persisted coordinate, startup default, or background polling. */
public final class ManualLocation {
    private static final Object lock=new Object();
    private static final LinkedHashSet<Request> waiting=new LinkedHashSet<>();
    private static Handler main;
    private static long nextId,activeId;
    private static Location supplied;
    private static boolean denied,closed;
    private ManualLocation(){}
    public static final class Request implements AutoCloseable {
        private Consumer<Location> callback;
        private Request(Consumer<Location> callback){this.callback=callback;}
        @Override public void close(){
            long cancel=0;
            synchronized(lock){callback=null;waiting.remove(this);if(waiting.isEmpty()&&activeId!=0){cancel=activeId;activeId=0;}}
            if(cancel!=0){long id=cancel;main.post(()->NativeLocation.cancel(id));}
        }
        private void deliver(Location value){
            Consumer<Location> notify;
            synchronized(lock){notify=callback;callback=null;}
            if(notify!=null)notify.accept(value==null?null:new Location(value));
        }
    }
    public static boolean isGranted(){synchronized(lock){return supplied!=null&&!closed;}}
    public static boolean wasDenied(){synchronized(lock){return denied;}}
    public static boolean isLocationPermission(String permission){return "android.permission.ACCESS_FINE_LOCATION".equals(permission)||"android.permission.ACCESS_COARSE_LOCATION".equals(permission);}
    public static Location last(String provider){
        synchronized(lock){if(supplied==null||closed)return null;Location result=new Location(supplied);result.setProvider(provider);return result;}
    }
    private static Handler handler(){
        synchronized(lock){if(main==null){main=new Handler(Looper.getMainLooper());if(!maclauncher.android.framework.HostLooper.addShutdownListener(main.getLooper(),ManualLocation::hostShutdown))closed=true;}return main;}
    }
    public static Request request(Consumer<Location> callback){
        java.util.Objects.requireNonNull(callback);Handler dispatcher=handler();Request request=new Request(callback);
        long start=0;Location immediate=null;boolean deliver;
        synchronized(lock){
            deliver=closed||denied||supplied!=null;
            if(deliver)immediate=supplied;
            else {waiting.add(request);if(activeId==0){activeId=++nextId;start=activeId;}}
        }
        if(deliver){Location value=immediate;dispatcher.post(()->request.deliver(value));}
        if(start!=0){long id=start;dispatcher.post(()->{
            synchronized(lock){if(activeId!=id||closed)return;}
            try{NativeLocation.request(id);}catch(RuntimeException|LinkageError error){complete(id,null);System.err.println("[android-host] Manual location dialog is unavailable: "+error.getClass().getSimpleName());}
        });}
        return request;
    }
    /** Called only by the native dialog for this outstanding request. */
    public static void complete(long id,double[] coordinates){
        Request[] requests;Location value=null;
        synchronized(lock){
            if(id==0||id!=activeId||closed)return;
            if(coordinates!=null&&coordinates.length==3&&Double.isFinite(coordinates[0])&&Math.abs(coordinates[0])<=90&&
                Double.isFinite(coordinates[1])&&Math.abs(coordinates[1])<=180&&Double.isFinite(coordinates[2])&&coordinates[2]>=0&&coordinates[2]<=1_000_000){
                value=new Location("manual");value.setLatitude(coordinates[0]);value.setLongitude(coordinates[1]);value.setAccuracy((float)coordinates[2]);
                value.setTime(System.currentTimeMillis());value.setElapsedRealtimeNanos(android.os.SystemClock.elapsedRealtimeNanos());value.setMock(true);
                supplied=value;
            }else denied=true;
            activeId=0;requests=waiting.toArray(Request[]::new);waiting.clear();
        }
        maclauncher.android.framework.HostAppOps.permissionsChanged();
        Location result=value;for(Request request:requests)main.post(()->request.deliver(result));
    }
    public static void hostShutdown(){
        long cancel;
        synchronized(lock){closed=true;supplied=null;cancel=activeId;activeId=0;for(Request request:waiting)request.callback=null;waiting.clear();}
        if(cancel!=0)NativeLocation.cancel(cancel);
    }
}
