package maclauncher.android.framework;

import android.app.PendingIntent;
import android.app.BroadcastOptions;
import android.content.Context;
import android.content.Intent;
import android.net.*;
import android.os.*;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.*;

/** Passive macOS route service beneath the original AOSP ConnectivityManager client. */
public final class HostConnectivityManager {
 private HostConnectivityManager(){}
 public interface Source{
  long snapshot();long start(long epoch);void stop(long epoch);default void close(){}
  default void listener(java.util.function.BiConsumer<Long,Long> callback){}
  default void network(Network value,int state){}
  default boolean continuous(){return false;}
 }
 private static final Object lock=new Object();
 private static final Map<Integer,Registration> registrations=new LinkedHashMap<>();
 private static Source source=new Source(){boolean used;private void load(){NativeLibrary.load();used=true;}public long snapshot(){load();return nativeSnapshot();}public long start(long e){load();return nativeStart(e);}public void stop(long e){nativeStop(e);}public void close(){if(used)nativeClose();}};
 private static Handler dispatcher;
 private static Context serviceContext;
 private static boolean installed,hostClosed,networkAllowed=true;
 private static int nextRequest;
 private static long epochCounter,epoch,revision=-1,identity;
 private static int state=-1;
 private static Network network;
 private static IConnectivityManager delegated;
 private static final class NativeLibrary {static{String p=System.getProperty("klepton.bridge");if(p==null)System.loadLibrary("klepton-jvm");else System.load(p);}static void load(){}}
 private static native long nativeSnapshot();private static native long nativeStart(long epoch);private static native void nativeStop(long epoch);private static native void nativeClose();
 private static final Binder binder=new Binder();
 private static final IConnectivityManager endpoint=(IConnectivityManager)Proxy.newProxyInstance(IConnectivityManager.class.getClassLoader(),new Class<?>[]{IConnectivityManager.class},HostConnectivityManager::invoke);
 static{binder.attachInterface(endpoint,"android.net.IConnectivityManager");}
 public static void configure(boolean allowed){synchronized(lock){checkOpen();if(installed)throw new IllegalStateException("Network policy must be configured before application services");networkAllowed=allowed;}}
 /** Alternate VM representation of the same passive physical source. */
 public static void configure(boolean allowed,Source physical){synchronized(lock){configure(allowed);source=Objects.requireNonNull(physical);source.listener(HostConnectivityManager::changed);}}
 /** Additional processes borrow the application's existing service identity. */
 public static void configure(boolean allowed,IConnectivityManager owner){synchronized(lock){configure(allowed);delegated=Objects.requireNonNull(owner);}}
 public static void install(Context context){synchronized(lock){
  checkOpen();if(installed)return;
  dispatcher=new Handler(context.getMainLooper());serviceContext=context;
  HostServices.register(Context.CONNECTIVITY_SERVICE,service(),"android.net.IConnectivityManager");
  if(delegated==null&&source.continuous()){epoch=++epochCounter;refresh(source.start(epoch));}
  HostLooper.addShutdownListener(context.getMainLooper(),HostConnectivityManager::hostClose);installed=true;
 }}
 public static IConnectivityManager service(){return delegated==null?endpoint:delegated;}
 static boolean metered(int value){return value<0||(value&1)==0||(value&2)!=0;}
 private static void checkOpen(){if(hostClosed)throw new IllegalStateException("Network observation is closed");}
 private static NetworkInfo info(){return network==null?null:HostNetworkValues.info(state);}
 private static NetworkCapabilities capabilities(){return HostNetworkValues.capabilities(state);}
 private static boolean current(Object value){return network!=null&&network.equals(value);}
 private static boolean currentType(Object value){return network!=null&&info().getType()==(int)value;}
 private static NetworkInfo typeInfo(int type){return HostNetworkValues.supports(type,state)?HostNetworkValues.info(type,state):null;}
 private static NetworkInfo[] allTypeInfo(){return java.util.stream.IntStream.of(ConnectivityManager.TYPE_MOBILE,ConnectivityManager.TYPE_WIFI,ConnectivityManager.TYPE_ETHERNET).mapToObj(HostConnectivityManager::typeInfo).filter(Objects::nonNull).toArray(NetworkInfo[]::new);}
 private static void refresh(long packed){
  long next=packed>>>32;if(next<revision)return;int value=(int)packed;
  if(!networkAllowed&&value>=0)value&=~1;if(next==revision&&state==value)return;
  Network observed=network;
  if(value!=state){if(value>=0&&(value&1)!=0){if(network==null||((value^state)&28)!=0)observed=new Network(Math.toIntExact(++identity));}else observed=null;}
  // Publish a generation only after its original resolver configuration has
  // succeeded. A failed physical update remains retryable at the same revision.
  source.network(observed,value);network=observed;state=value;revision=next;
 }
 private static Object invoke(Object proxy,Method method,Object[] arguments)throws Throwable{
  Object[] args=arguments==null?new Object[0]:arguments;String name=method.getName();
  if(method.getDeclaringClass()==Object.class)return switch(name){case "toString"->"Local Android connectivity service";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==args[0];default->throw new AssertionError(name);};
  if(name.equals("asBinder"))return binder;
  synchronized(lock){
   checkOpen();
   // This endpoint implements the established Messenger protocol without the
   // optional declared-callback mask optimization.
   if(name.equals("getEnabledConnectivityManagerFeatures"))return 0L;
   if(name.equals("releaseNetworkRequest")){Registration item=registrations.get(((NetworkRequest)Objects.requireNonNull(args[0])).requestId);if(item!=null)retire(item);return null;}
   if(name.equals("releasePendingNetworkRequest")){retirePending((PendingIntent)Objects.requireNonNull(args[0]));return null;}
   if(name.equals("pendingRequestForNetwork")||name.equals("pendingListenForNetwork")){
    PendingIntent pending=(PendingIntent)Objects.requireNonNull(args[1]);retirePending(pending);
    NetworkRequest request=register((NetworkCapabilities)args[0],name.equals("pendingRequestForNetwork")?NetworkRequest.Type.REQUEST:NetworkRequest.Type.LISTEN,null,null,pending,ConnectivityManager.TYPE_NONE,0);
    return name.equals("pendingRequestForNetwork")?request:null;
   }
   if(name.equals("requestNetwork"))return register((NetworkCapabilities)args[1],NetworkRequest.Type.values()[(int)args[2]],(Messenger)args[3],(IBinder)args[5],null,(int)args[6],(int)args[4]);
   if(name.equals("listenForNetwork"))return register((NetworkCapabilities)args[0],NetworkRequest.Type.LISTEN,(Messenger)args[1],(IBinder)args[2],null,ConnectivityManager.TYPE_NONE,0);
   refresh(source.snapshot());
   return switch(name){
    case "getActiveNetwork","getActiveNetworkForUid" -> network;
    case "getActiveNetworkInfo","getActiveNetworkInfoForUid" -> info();
    case "getNetworkInfo" -> typeInfo((int)args[0]);
    case "getNetworkInfoForUid" -> current(args[0])?info():null;
    case "getNetworkCapabilities" -> current(args[0])?capabilities():null;
    case "isActiveNetworkMetered" -> metered(state);
    case "getAllNetworks" -> network==null?new Network[0]:new Network[]{network};
    case "getAllNetworkInfo" -> allTypeInfo();
    case "getActiveLinkProperties" -> network==null?null:new LinkProperties();
    case "getLinkProperties" -> current(args[0])?new LinkProperties():null;
    case "getLinkPropertiesForType" -> currentType(args[0])?new LinkProperties():null;
    case "isDefaultNetworkActive" -> network!=null;
    case "getGlobalProxy","getProxyForNetwork" -> null;
    case "getRestrictBackgroundStatusByCaller" -> ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED;
    case "requestBandwidthUpdate" -> false;
    // Passive observations cannot validate or modify the host's network.
    case "reportNetworkConnectivity","reportInetCondition" -> null;
    default -> throw new RemoteException("Android connectivity operation is unavailable: "+name);
   };
  }
 }
 private static NetworkRequest register(NetworkCapabilities requested,NetworkRequest.Type type,Messenger messenger,IBinder token,PendingIntent pending,int legacy,int timeout)throws RemoteException{
  if(pending==null){Objects.requireNonNull(messenger);Objects.requireNonNull(token);}
  if(dispatcher==null)throw new IllegalStateException("Network service is not installed");
  if(registrations.size()>=100)throw new ServiceSpecificException(1,"Too many network requests");
  if(nextRequest==Integer.MAX_VALUE)throw new ServiceSpecificException(1,"Network request identity capacity exceeded");
  if(timeout<0)throw new IllegalArgumentException("Negative network request timeout");
  NetworkCapabilities wanted=requested==null?new NetworkCapabilities():new NetworkCapabilities(requested);
  if(requested==null)wanted.clearAll();
  NetworkRequest request=new NetworkRequest(wanted,legacy,++nextRequest,type);
  Registration item=new Registration(request,messenger,token,pending);boolean first=registrations.isEmpty();
  registrations.put(request.requestId,item);
  try{
   if(token!=null)token.linkToDeath(item,0);
   if(first&&epoch==0){epoch=++epochCounter;refresh(source.start(epoch));}else refresh(source.snapshot());
   item.queue();
   if(timeout>0&&!dispatcher.postDelayed(item.timeout,timeout))throw new IllegalStateException("Network service queue is closed");
   return request;
  }catch(RemoteException|RuntimeException|Error failure){try{retire(item);}catch(Throwable cleanup){if(cleanup!=failure)failure.addSuppressed(cleanup);}throw failure;}
 }
 private static final class Registration implements Runnable,IBinder.DeathRecipient {
  final NetworkRequest request;
  final Messenger messenger;
  final IBinder token;
  final PendingIntent pending;
  boolean queued,pendingSent;
  Network delivered;
  int deliveredState=-1;
  final Runnable timeout=()->{synchronized(lock){if(!active()||delivered!=null)return;try{send(ConnectivityManager.CALLBACK_UNAVAIL,null,0);}catch(RemoteException gone){}finally{retire(this);}}};
  final Runnable pendingRelease=()->{synchronized(lock){retire(this);}};
  Registration(NetworkRequest request,Messenger messenger,IBinder token,PendingIntent pending){this.request=request;this.messenger=messenger;this.token=token;this.pending=pending;}
  boolean active(){return registrations.get(request.requestId)==this;}
  boolean tracksDefault(){return request.type==NetworkRequest.Type.TRACK_DEFAULT||request.type==NetworkRequest.Type.TRACK_SYSTEM_DEFAULT;}
  Network matching(){return network!=null&&(tracksDefault()||request.networkCapabilities.satisfiedByNetworkCapabilities(capabilities()))?network:null;}
  void queue(){if(queued)return;queued=true;if(!dispatcher.post(this)){queued=false;throw new IllegalStateException("Network service queue is closed");}}
  void send(int what,Network value,int snapshot)throws RemoteException{
   if(pending!=null){
    // Original ConnectivityService delivers only the first AVAILABLE via this
    // API, then releases the request after its receiver finishes. Messenger
    // registrations retain their independent continuous callback protocol.
    if(what!=ConnectivityManager.CALLBACK_AVAILABLE||pendingSent)return;
    pendingSent=true;
    Intent intent=new Intent().putExtra(ConnectivityManager.EXTRA_NETWORK,value).putExtra(ConnectivityManager.EXTRA_NETWORK_REQUEST,new NetworkRequest(request));
    BroadcastOptions options=BroadcastOptions.makeBasic();options.setPendingIntentBackgroundActivityLaunchAllowed(false);
    try{pending.send(serviceContext,0,intent,(operation,sent,result,data,extras)->{synchronized(lock){if(active()&&!dispatcher.postDelayed(pendingRelease,5000))retire(this);}},dispatcher,null,options.toBundle());}
    catch(PendingIntent.CanceledException canceled){retire(this);}
    return;
   }
   Message message=Message.obtain();message.what=what;
   Bundle data=new Bundle();data.putParcelable(NetworkRequest.class.getSimpleName(),new NetworkRequest(request));
   if(value!=null)data.putParcelable(Network.class.getSimpleName(),value);
   if(what==ConnectivityManager.CALLBACK_AVAILABLE||what==ConnectivityManager.CALLBACK_CAP_CHANGED)data.putParcelable(NetworkCapabilities.class.getSimpleName(),HostNetworkValues.capabilities(snapshot));
   if(what==ConnectivityManager.CALLBACK_AVAILABLE)data.putParcelable(LinkProperties.class.getSimpleName(),new LinkProperties());
   message.setData(data);messenger.send(message);
  }
  @Override public void run(){synchronized(lock){
   queued=false;if(!active())return;Network previous=delivered,current=matching();int previousState=deliveredState;
   delivered=current;deliveredState=state;
   try{
    if(previous!=null&&!previous.equals(current)&&(!tracksDefault()||current==null))send(ConnectivityManager.CALLBACK_LOST,previous,state);
    if(current!=null&&!current.equals(previous)){dispatcher.removeCallbacks(timeout);send(ConnectivityManager.CALLBACK_AVAILABLE,current,state);}
    else if(current!=null&&state!=previousState)send(ConnectivityManager.CALLBACK_CAP_CHANGED,current,state);
   }catch(RemoteException gone){retire(this);}
  }}
  @Override public void binderDied(){synchronized(lock){retire(this);}}
 }
 private static void retire(Registration item){
  if(!registrations.remove(item.request.requestId,item))return;
  dispatcher.removeCallbacks(item);dispatcher.removeCallbacks(item.timeout);dispatcher.removeCallbacks(item.pendingRelease);item.queued=false;
  if(item.token!=null)item.token.unlinkToDeath(item,0);
  if(registrations.isEmpty()&&epoch!=0&&!source.continuous()){long old=epoch;epoch=0;source.stop(old);}
 }
 private static void retirePending(PendingIntent pending){for(Registration item:new ArrayList<>(registrations.values()))if(item.pending!=null&&pending.intentFilterEquals(item.pending))retire(item);}
 private static void changed(long eventEpoch,long snapshot){synchronized(lock){
  if(hostClosed||eventEpoch==0||eventEpoch!=epoch||(snapshot>>>32)<revision)return;
  refresh(snapshot);
  for(Registration item:new ArrayList<>(registrations.values()))try{item.queue();}catch(RuntimeException failure){retire(item);System.err.println("[android-host] Network notification could not be queued: "+failure.getClass().getSimpleName());}
 }}
 /** Retire the passive native observer without manufacturing client loss events. */
 public static void hostClose(){synchronized(lock){
  if(hostClosed)return;hostClosed=true;Throwable first=null;
  for(Registration item:new ArrayList<>(registrations.values()))try{retire(item);}catch(Throwable failure){if(first==null)first=failure;else if(first!=failure)first.addSuppressed(failure);}
  try{if(delegated==null)source.close();}catch(Throwable failure){if(first==null)first=failure;else if(first!=failure)first.addSuppressed(failure);}
  if(installed)HostServices.unregister(Context.CONNECTIVITY_SERVICE,service(),"android.net.IConnectivityManager");
  serviceContext=null;
  if(first instanceof Error error)throw error;if(first instanceof RuntimeException error)throw error;if(first!=null)throw new IllegalStateException(first);
 }}
}
