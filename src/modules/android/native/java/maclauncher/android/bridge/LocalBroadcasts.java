package maclauncher.android.bridge;

import android.app.ActivityManager;
import android.app.ActivityThread;
import android.app.IApplicationThread;
import android.content.*;
import android.content.pm.*;
import android.content.res.CompatibilityInfo;
import android.os.*;
import java.util.*;
import java.util.concurrent.*;
import maclauncher.android.framework.*;

/** Broadcast destinations; original LoadedApk and ActivityThread own receiver execution. */
public final class LocalBroadcasts {
    private LocalBroadcasts(){}
    private record Subscription(IntentFilter filter,String permission,int flags){}
    private record Target(IApplicationThread application,IIntentReceiver receiver,ActivityInfo component,boolean remote) {
        Target(IApplicationThread application,IIntentReceiver receiver){this(application,receiver,null,false);}
        IBinder completion(){return remote||application==null?receiver.asBinder():application.asBinder();}
    }
    private record Ranked(Target target,int priority){}
    private static final class Registration {final Target target;final ArrayList<Subscription> filters=new ArrayList<>();Registration(IApplicationThread application,IIntentReceiver receiver){target=new Target(application,receiver);}}
    private static final Map<IBinder,Registration> receivers=new LinkedHashMap<>();
    private static final Map<Intent.FilterComparison,Intent> sticky=new LinkedHashMap<>();
    private static final ArrayDeque<Ordered> ordered=new ArrayDeque<>();
    private static Handler handler;
    private static ExecutorService routing;
    private static boolean closed;
    private static final Runnable advance=LocalBroadcasts::advance;
    private static final class Ordered {final Intent intent;final List<Target> targets;final Target result;final int user,uid;final String sender,type;int index,code;String data;Bundle extras;IBinder waiting;Ordered(Intent intent,String type,List<Target> targets,Target result,int code,String data,Bundle extras,int user,String sender,int uid){this.type=type;this.uid=uid;this.sender=sender;this.intent=intent;this.targets=targets;this.result=result;this.code=code;this.data=data;this.extras=extras;this.user=user;}}
    private static Handler handler(){if(handler==null)handler=new Handler(Looper.getMainLooper());return handler;}
    public static synchronized Intent register(IApplicationThread application,IIntentReceiver receiver,IntentFilter filter,String permission,int flags){
        Objects.requireNonNull(filter);if(closed)return null;if((flags&Context.RECEIVER_EXPORTED)!=0&&(flags&Context.RECEIVER_NOT_EXPORTED)!=0)throw new IllegalArgumentException("Receiver cannot be both exported and not exported");if(receiver!=null){Registration registration=receivers.get(receiver.asBinder());if(registration==null){registration=new Registration(application,receiver);receivers.put(receiver.asBinder(),registration);}registration.filters.add(new Subscription(new IntentFilter(filter),permission,flags));}
        for(Intent intent:sticky.values())if(filter.match(intent.getAction(),intent.getType(),intent.getScheme(),intent.getData(),intent.getCategories(),"Android")>=0)return new Intent(intent);return null;
    }
    public static synchronized void unregister(IIntentReceiver receiver){if(receiver!=null)receivers.remove(receiver.asBinder());}
    private static boolean permitted(int uid,String permission){return permission==null||ActivityManager.checkComponentPermission(permission,uid,-1,true)==PackageManager.PERMISSION_GRANTED;}
    private static boolean destination(int uid,String pkg,String[] permissions,String[] excludedPermissions,String[] excludedPackages){
        if(excludedPackages!=null&&Arrays.asList(excludedPackages).contains(pkg))return false;
        if(permissions!=null)for(String permission:permissions)if(!permitted(uid,permission))return false;
        if(excludedPermissions!=null)for(String permission:excludedPermissions)if(permitted(uid,permission))return false;
        return true;
    }
    public static int send(Context context,IApplicationThread application,Intent source,String type,IIntentReceiver result,int code,String data,Bundle extras,String[] permissions,String[] excludedPermissions,String[] excludedPackages,boolean serialize,boolean remember,int user){
        Intent intent=new Intent(Objects.requireNonNull(source));List<Registration> snapshot;
        var peer=HostBinderRpc.callingPeer();String sender=peer==null?context.getPackageName():peer.packageName();int uid=peer==null?android.os.Process.myUid():peer.uid();
        synchronized(LocalBroadcasts.class){if(closed)return 0;if(remember)sticky.put(new Intent.FilterComparison(intent),new Intent(intent));snapshot=new ArrayList<>(receivers.values());}
        ArrayList<Ranked> selected=new ArrayList<>();
        if(intent.getComponent()==null&&(intent.getPackage()==null||context.getPackageName().equals(intent.getPackage()))&&destination(android.os.Process.myUid(),context.getPackageName(),permissions,excludedPermissions,excludedPackages))
            for(Registration registration:snapshot){List<Subscription> filters;synchronized(LocalBroadcasts.class){filters=List.copyOf(registration.filters);}Integer priority=null;for(Subscription subscription:filters)if((uid==android.os.Process.myUid()||uid==android.os.Process.SYSTEM_UID||uid==android.os.Process.ROOT_UID||(subscription.flags&Context.RECEIVER_NOT_EXPORTED)==0)&&permitted(uid,subscription.permission)&&subscription.filter.match(intent.getAction(),type,intent.getScheme(),intent.getData(),intent.getCategories(),"Android")>=0)priority=priority==null?subscription.filter.getPriority():Math.max(priority,subscription.filter.getPriority());if(priority!=null)selected.add(new Ranked(registration.target,priority));}
        if((intent.getFlags()&Intent.FLAG_RECEIVER_REGISTERED_ONLY)==0){
            Intent resolution=new Intent(intent);if(type!=null)resolution.setDataAndType(intent.getData(),type);
            for(ResolveInfo match:context.getPackageManager().queryBroadcastReceivers(resolution,PackageManager.GET_META_DATA)){
                ActivityInfo info=match.activityInfo;if(info==null||!info.enabled||!destination(info.applicationInfo.uid,info.packageName,permissions,excludedPermissions,excludedPackages))continue;
                try{if(!HostPackages.permitsReceiver(sender,info))continue;}catch(Exception failure){throw new SecurityException("Android receiver permission could not be verified",failure);}
                boolean remote=!info.packageName.equals(context.getPackageName())||!Objects.equals(info.processName==null?info.packageName:info.processName,HostContext.hostProcessName());
                // A remote endpoint delivers only its own resolved component.
                // The initiating process owns fan-out and result ordering.
                if(remote&&peer!=null)continue;
                IIntentReceiver completion=remote?new IIntentReceiver.Stub(){@Override public void performReceive(Intent ignored,int resultCode,String resultData,Bundle resultExtras,boolean isOrdered,boolean isSticky,int sendingUser){finish(asBinder(),resultCode,resultData,resultExtras,false);}}:null;
                IApplicationThread target=remote?application:ActivityThread.currentActivityThread().getApplicationThread();
                selected.add(new Ranked(new Target(target,completion,new ActivityInfo(info),remote),match.priority));
            }
        }
        selected.sort(Comparator.comparingInt(Ranked::priority).reversed());List<Target> targets=selected.stream().map(Ranked::target).toList();
        // Remote completion belongs to its original IIntentReceiver, not to the
        // destination application's LoadedApk receiver dispatcher.
        Target finished=result==null?null:new Target(peer==null?application:null,result);
        if(serialize){synchronized(LocalBroadcasts.class){if(closed)return 0;ordered.add(new Ordered(intent,type,targets,finished,code,data,extras,user,sender,uid));if(ordered.size()==1)handler().post(advance);}}
        else{for(Target target:targets)deliver(target,intent,type,code,data,extras,false,remember,user,sender,uid);if(finished!=null)deliver(finished,intent,type,code,data,extras,false,remember,user,sender,uid);}return 0;
    }
    private static void deliver(Target target,Intent intent,String type,int code,String data,Bundle extras,boolean ordered,boolean sticky,int user,String sender,int uid){
        try{
            Intent copy=new Intent(intent);Bundle result=extras==null?null:new Bundle(extras);
            if(target.component!=null){
                copy.setComponent(new ComponentName(target.component.packageName,target.component.name));
                if(target.remote){
                    synchronized(LocalBroadcasts.class){if(closed)return;if(routing==null)routing=Executors.newCachedThreadPool(task->{Thread thread=new Thread(task,"Android broadcast routing");thread.setDaemon(true);return thread;});routing.execute(()->{
                        try{if(!HostRemoteServices.broadcast(target.component,target.application,copy,type,ordered?target.receiver:null,code,data,result,ordered,user)&&ordered)finish(target.completion(),code,data,result,false);}
                        catch(RemoteException|RuntimeException failure){System.err.println("[android-host] Broadcast destination could not be reached: "+target.component.getComponentName()+": "+failure);if(ordered)finish(target.completion(),code,data,result,false);}
                    });}
                }else target.application.scheduleReceiver(copy,target.component,CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,code,data,result,ordered,!ordered,user,ActivityManager.PROCESS_STATE_TOP,uid,sender);
            }else if(target.application!=null)target.application.scheduleRegisteredReceiver(target.receiver,copy,code,data,result,ordered,sticky,!ordered,user,ActivityManager.PROCESS_STATE_TOP,uid,sender);
            else target.receiver.performReceive(copy,code,data,result,ordered,sticky,user);
        }catch(RemoteException gone){unregister(target.receiver);if(ordered)finish(target.completion(),code,data,extras,false);}
    }
    private static void advance(){Ordered delivery;Target target;synchronized(LocalBroadcasts.class){delivery=ordered.peek();if(closed||delivery==null||delivery.waiting!=null)return;if(delivery.index>=delivery.targets.size()){ordered.remove();target=delivery.result;}else{target=delivery.targets.get(delivery.index++);IBinder completion=target.completion();delivery.waiting=completion;handler().postDelayed(()->expire(delivery,completion),delivery,10000);}}
        if(delivery.waiting!=null)deliver(target,delivery.intent,delivery.type,delivery.code,delivery.data,delivery.extras,true,false,delivery.user,delivery.sender,delivery.uid);
        else{if(target!=null)deliver(target,delivery.intent,delivery.type,delivery.code,delivery.data,delivery.extras,false,false,delivery.user,delivery.sender,delivery.uid);synchronized(LocalBroadcasts.class){if(!closed)handler().post(advance);}}
    }
    private static synchronized void expire(Ordered delivery,IBinder receiver){if(ordered.peek()==delivery&&delivery.waiting==receiver)finish(receiver,delivery.code,delivery.data,delivery.extras,false);}
    public static synchronized void finish(IBinder receiver,int code,String data,Bundle extras,boolean abort){Ordered delivery=ordered.peek();if(closed||delivery==null||delivery.waiting!=receiver)return;handler().removeCallbacksAndMessages(delivery);delivery.waiting=null;delivery.code=code;delivery.data=data;delivery.extras=extras;if(abort&&(delivery.intent.getFlags()&Intent.FLAG_RECEIVER_NO_ABORT)==0)delivery.index=delivery.targets.size();handler().post(advance);}
    public static synchronized void removeSticky(Intent intent){sticky.remove(new Intent.FilterComparison(intent));}
    public static synchronized void shutdown(){closed=true;if(handler!=null){for(Ordered delivery:ordered)handler.removeCallbacksAndMessages(delivery);handler.removeCallbacks(advance);}if(routing!=null){routing.shutdownNow();routing=null;}ordered.clear();receivers.clear();sticky.clear();}
}
