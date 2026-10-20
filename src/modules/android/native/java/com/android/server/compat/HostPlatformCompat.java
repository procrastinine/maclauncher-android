package com.android.server.compat;

import android.app.ActivityThread;
import android.content.*;
import android.content.pm.PackageManager;
import android.os.*;
import com.android.internal.compat.AndroidBuildClassifier;
import com.android.internal.compat.ChangeReporter;
import com.android.server.compat.config.XmlParser;
import java.nio.file.*;
import java.util.*;
import java.util.jar.JarFile;
import maclauncher.android.framework.HostLooper;
import maclauncher.android.framework.HostServices;

/** Owns the original compatibility service and the generated configs supplied
 * alongside its framework classes. No platform change definitions are invented.
 */
public final class HostPlatformCompat implements AutoCloseable {
    private static HostPlatformCompat current;
    private final ServiceContext context;
    private final PlatformCompat service;
    private boolean closed,bound;

    private HostPlatformCompat(Context parent,List<Path> directories,boolean deferReceivers)throws Exception {
        context=new ServiceContext(parent.createAttributionContext(parent.getAttributionTag()),deferReceivers);
        try {
            var classifier=new AndroidBuildClassifier();
            var config=new CompatConfig(classifier,context);
            // Android scans partition/APEX directories. These original generated
            // resources are packaged with the complete framework on this host.
            if(directories!=null){
                for(Path directory:directories)config.initConfigFromLib(directory.toFile());
            }else {
              Path archive=Path.of(PlatformCompat.class.getProtectionDomain().getCodeSource().getLocation().toURI());
              try(var jar=new JarFile(archive.toFile())) {
                var entries=jar.stream().filter(entry->!entry.isDirectory()&&entry.getName().endsWith("_compat_config.xml"))
                    .sorted(Comparator.comparing(java.util.jar.JarEntry::getName)).toList();
                if(entries.isEmpty())throw new IllegalStateException("Android compatibility definitions are missing");
                for(var entry:entries)try(var input=jar.getInputStream(entry)) {
                    var parsed=XmlParser.read(input);
                    if(parsed==null)throw new IllegalStateException("Invalid Android compatibility definitions");
                    for(var change:parsed.getCompatChange())config.addChange(new CompatChange(change));
                }
              }
            }
            service=new PlatformCompat(context,config,classifier,new ChangeReporter(ChangeReporter.SOURCE_SYSTEM_SERVER));
        }catch(Exception|Error failure){try{context.close();}catch(RuntimeException cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }

    /** Package policy queries compatibility before application records exist.
     * Use the original system ContextImpl without binding an application thread.
     * The original receiver is activated after ActivityManager is installed. */
    public static synchronized void prepare(){prepare(null);}
    public static synchronized void prepare(List<Path> directories){
        if(current!=null)return;
        try {
            var thread=ActivityThread.currentActivityThread();
            if(thread==null){var constructor=ActivityThread.class.getDeclaredConstructor();constructor.setAccessible(true);thread=constructor.newInstance();}
            open(thread.getSystemContext(),directories,true);
        }catch(RuntimeException|Error failure){throw failure;}
        catch(Exception failure){throw new IllegalStateException("Android compatibility service could not prepare",failure);}
    }
    private static void open(Context context,List<Path> directories,boolean deferReceivers){
        HostPlatformCompat owner;
        try{owner=new HostPlatformCompat(context,directories==null?null:List.copyOf(directories),deferReceivers);}
        catch(RuntimeException|Error failure){throw failure;}
        catch(Exception failure){throw new IllegalStateException("Android compatibility service could not start",failure);}
        try {
            HostServices.register("platform_compat",owner.service,"com.android.internal.compat.IPlatformCompat");
            if(!HostLooper.addShutdownListener(context.getMainLooper(),owner::close))
                throw new IllegalStateException("Android compatibility service has stopped");
            current=owner;
        }catch(RuntimeException|Error failure){try{owner.close();}catch(RuntimeException cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }
    public static synchronized void install(Context context){install(context,null);}
    public static synchronized void install(Context context,List<Path> directories){
        if(current==null)open(context,directories,false);
        HostPlatformCompat owner=current;
        if(owner.bound)return;
        try {
            owner.context.activate(context);
            // ActivityThread normally receives these arrays from the system
            // server at bindApplication. Bind the original process delegate to
            // the same original generated change definitions and target SDK.
            var application=context.getApplicationInfo();
            android.app.AppCompatCallbacks.install(owner.service.getDisabledChanges(application),
                owner.service.getLoggableChanges(application));
            owner.bound=true;
        }catch(RuntimeException|Error failure){try{owner.close();}catch(RuntimeException cleanup){failure.addSuppressed(cleanup);}throw failure;}
    }

    @Override public synchronized void close() {
        if(closed)return;
        closed=true;
        HostServices.unregister("platform_compat",service,"com.android.internal.compat.IPlatformCompat");
        if(bound)android.compat.Compatibility.clearBehaviorChangeDelegate();
        try{context.close();}finally{synchronized(HostPlatformCompat.class){if(current==this)current=null;}}
    }

    /** Local system-service reads do not grant application permissions. Remote
     * callers and every override mutation retain the ordinary permission checks.
     */
    private static final class ServiceContext extends ContextWrapper implements AutoCloseable {
        private record PendingReceiver(BroadcastReceiver receiver,IntentFilter filter,String permission,Handler handler){}
        private final Map<BroadcastReceiver,Context> receivers=new HashMap<>();
        private final List<PendingReceiver> pending=new ArrayList<>();
        private final PermissionEnforcer permissions;
        private Context receiverContext;
        ServiceContext(Context base,boolean deferReceivers) {
            super(base);
            receiverContext=deferReceivers?null:base;
            permissions=new PermissionEnforcer(base) {
                private boolean localRead(String permission,int pid,int uid) {
                    return pid==android.os.Process.myPid()&&uid==android.os.Process.myUid()&&
                        ("android.permission.READ_COMPAT_CHANGE_CONFIG".equals(permission)||
                         "android.permission.LOG_COMPAT_CHANGE".equals(permission));
                }
                @Override protected int checkPermission(String permission,int pid,int uid) {
                    return localRead(permission,pid,uid)?PackageManager.PERMISSION_GRANTED:super.checkPermission(permission,pid,uid);
                }
                @Override protected int checkPermission(String permission,AttributionSource source) {
                    return source!=null&&localRead(permission,Binder.getCallingPid(),source.getUid())?
                        PackageManager.PERMISSION_GRANTED:super.checkPermission(permission,source);
                }
            };
        }
        @Override public Object getSystemService(String name) {
            return "permission_enforcer".equals(name)?permissions:super.getSystemService(name);
        }
        @Override public synchronized Intent registerReceiverForAllUsers(BroadcastReceiver receiver,IntentFilter filter,String permission,Handler handler) {
            if(receiverContext==null){pending.add(new PendingReceiver(receiver,new IntentFilter(filter),permission,handler));return null;}
            Intent result=receiverContext.registerReceiverForAllUsers(receiver,filter,permission,handler);
            receivers.put(receiver,receiverContext);return result;
        }
        synchronized void activate(Context context){
            if(receiverContext!=null)return;
            receiverContext=context.createAttributionContext(context.getAttributionTag());
            for(var registration:pending)registerReceiverForAllUsers(registration.receiver(),registration.filter(),registration.permission(),registration.handler());
            pending.clear();
        }
        @Override public void close() {
            Map<BroadcastReceiver,Context> owned;
            synchronized(this){owned=Map.copyOf(receivers);receivers.clear();pending.clear();}
            RuntimeException failure=null;
            for(var entry:owned.entrySet())try{entry.getValue().unregisterReceiver(entry.getKey());}catch(RuntimeException error){if(failure==null)failure=error;else failure.addSuppressed(error);}
            if(failure!=null)throw failure;
        }
    }
}
