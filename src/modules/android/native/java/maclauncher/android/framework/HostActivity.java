package maclauncher.android.framework;

import android.app.*;
import android.content.*;
import android.content.pm.*;
import android.content.res.Configuration;
import android.os.*;
import android.view.*;
import java.util.*;
import java.lang.reflect.Method;

/** Native window owner driving the original Activity lifecycle and PhoneWindow implementation. */
public final class HostActivity {
    /** A VM supplies only its physical window. ActivityThread owns the task. */
    public interface Window extends AutoCloseable {
        void attachActivity(Activity activity);
        void present(Activity activity,String title,int width,int height);
        void stopCallbacks(Runnable ownerTasks);
        default void taskEmpty(){}
        default void resized(int width,int height){}
        default void hostState(boolean sleeping,boolean focused){}
        default IBinder capability(){return null;}
        @Override void close();
    }
    @FunctionalInterface public interface WindowBinding {
        Window open(String title,int width,int height,Runnable closeRequested,Bundle options);
    }
    private static final String WINDOW_CAPABILITY="maclauncher.android.window";
    private static WindowBinding windowBinding=(title,width,height,closeRequested,options)->{HostActivityManager.hostInitializeGraphics();return maclauncher.android.bridge.WindowSession.open(title,width,height,closeRequested);};
    private static volatile Window outputWindow;
    public static void windowBinding(WindowBinding binding){
        if(outputWindow!=null)throw new IllegalStateException("Activity output is already open");
        windowBinding=Objects.requireNonNull(binding);
    }
    public static Window openWindow(String title,int width,int height,Runnable closeRequested){return openWindow(title,width,height,closeRequested,null);}
    /** Retire an output while preserving the original presentation failure. */
    public static Throwable closeWindow(Throwable first,Runnable finish,Window session){
        first=attempt(first,finish);
        // A failed detach can leave a producer waiting on the display clock.
        // Retain the output graph until that producer has actually detached.
        try{HostWindowManager.detachPresentation();}
        catch(Throwable failure){return keep(first,failure);}
        first=attempt(first,()->session.stopCallbacks(()->HostLooper.drain(Looper.getMainLooper())));
        return attempt(first,session::close);
    }
    private static Window openWindow(String title,int width,int height,Runnable closeRequested,Bundle options){
        if(outputWindow!=null)throw new IllegalStateException("Activity output is already open");
        Window target=Objects.requireNonNull(windowBinding.open(title,width,height,closeRequested,options));
        Window owned=new Window(){
            public void attachActivity(Activity activity){target.attachActivity(activity);}
            public void present(Activity activity,String title,int width,int height){target.present(activity,title,width,height);}
            public void stopCallbacks(Runnable tasks){target.stopCallbacks(tasks);}
            public void taskEmpty(){target.taskEmpty();}
            public void resized(int width,int height){target.resized(width,height);}
            public void hostState(boolean sleeping,boolean focused){target.hostState(sleeping,focused);}
            public IBinder capability(){return target.capability();}
            public void close(){target.close();if(outputWindow==this)outputWindow=null;}
        };
        outputWindow=owned;return owned;
    }
    static void activateOutput(Activity activity){if(outputWindow!=null)outputWindow.attachActivity(activity);else maclauncher.android.bridge.WindowSession.activate(activity);}
    static Bundle windowOptions(Bundle original){
        Bundle result=original==null?new Bundle():new Bundle(original);
        IBinder capability=outputWindow==null?null:outputWindow.capability();
        if(capability!=null)result.putBinder(WINDOW_CAPABILITY,capability);
        return result;
    }
    public static IBinder windowCapability(Bundle options){return options==null?null:options.getBinder(WINDOW_CAPABILITY);}
    public static void outputResized(int width,int height){if(outputWindow!=null)outputWindow.resized(width,height);}
    static void outputStateChanged(){if(outputWindow!=null)outputWindow.hostState(hostSleeping,HostWindowManager.outputFocused());}
    private static void taskEmpty(){
        Window expected=outputWindow;
        if(top()!=null||expected==null)return;
        // A finishing launcher may already have queued its successor. Resolve
        // emptiness after those task requests, against the same window lease.
        new Handler(Looper.getMainLooper()).post(()->{if(outputWindow==expected&&top()==null)expected.taskEmpty();});
    }
    private static final Map<IBinder,HostActivity> activities=new IdentityHashMap<>();
    private static final ArrayList<HostActivity> task=new ArrayList<>();
    private static int pendingLaunches;
    private static int taskId=-1,nextTaskId;
    private static Boolean excludedFromRecents;
    private static final Controller controller=new Controller();
    private static volatile boolean hostSleeping;
    private final Thread owner=Thread.currentThread();
    private Activity activity;
    private final IBinder token;
    private final ActivityThread.ActivityClientRecord record;
    private boolean launched;
    private IApplicationThread resultCaller;
    private IBinder resultTo;private String resultWho;private int requestCode=-1,resultCode=Activity.RESULT_CANCELED;private Intent resultData;
    private boolean created,started,resumed,closed,resumeAfterSleep;
    private int platformDialogs;
    private volatile boolean closing;
    private volatile int orientation=ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
    private String title="";private long titleRevision;
    private static final String TOKEN_DESCRIPTOR="maclauncher.android.ActivityToken";
    /** Token metadata is read from its existing ActivityThread record. Holders
     * of the original capability can resolve a caller across process boundaries. */
    private static final class Token extends Binder {
        Token(){attachInterface(null,TOKEN_DESCRIPTOR);}
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
            if(code!=FIRST_CALL_TRANSACTION)return super.onTransact(code,data,reply,flags);
            data.enforceInterface(TOKEN_DESCRIPTOR);reply.writeNoException();reply.writeTypedObject(componentForLocalToken(this),0);return true;
        }
    }
    private static ComponentName componentForLocalToken(IBinder token){synchronized(activities){var state=activities.get(token);if(state==null)return null;if(state.activity!=null)return state.activity.getComponentName();var info=(ActivityInfo)HostUiAccess.get(state.record,ActivityThread.ActivityClientRecord.class,"activityInfo");return info==null?null:new ComponentName(info.packageName,info.name);}}
    private static ComponentName componentForToken(IBinder token){
        if(token==null)return null;if(token instanceof Binder)return componentForLocalToken(token);
        Parcel request=Parcel.obtain(),reply=Parcel.obtain();
        try{request.writeInterfaceToken(TOKEN_DESCRIPTOR);if(!token.transact(IBinder.FIRST_CALL_TRANSACTION,request,reply,0))return null;reply.readException();return reply.readTypedObject(ComponentName.CREATOR);}
        catch(RemoteException disconnected){return null;}finally{request.recycle();reply.recycle();}
    }
    private HostActivity(Activity activity){this.activity=activity;token=new Token();record=new ActivityThread.ActivityClientRecord();launched=false;}
    private HostActivity(ActivityThread.ActivityClientRecord record){this.record=record;token=record.token;launched=true;}
    private static HostActivity top(){synchronized(activities){return task.isEmpty()?null:task.getLast();}}
    private static void register(HostActivity state){synchronized(activities){if(task.isEmpty()){taskId=nextTaskId++;excludedFromRecents=null;}activities.put(state.token,state);task.add(state);}}
    static int currentTaskId(){synchronized(activities){return task.isEmpty()?-1:taskId;}}
    static ActivityManager.RecentTaskInfo taskInfo(int expected){synchronized(activities){
        if(task.isEmpty()||taskId!=expected)return null;
        HostActivity first=task.getFirst(),last=task.getLast();var info=new ActivityManager.RecentTaskInfo();
        var process=HostApplication.current();info.id=info.persistentId=info.taskId=taskId;
        info.userId=process.context.getUserId();info.effectiveUid=android.os.Process.myUid();info.isRunning=true;
        Intent original=(Intent)HostUiAccess.get(first.record,ActivityThread.ActivityClientRecord.class,"intent");
        info.baseIntent=original==null?new Intent():new Intent(original);
        if(excludedFromRecents!=null){if(excludedFromRecents)info.baseIntent.addFlags(Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS);else info.baseIntent.removeFlags(Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS);}
        info.baseActivity=componentForLocalToken(first.token);info.topActivity=componentForLocalToken(last.token);info.realActivity=info.baseActivity;
        info.numActivities=task.size();info.displayId=Display.DEFAULT_DISPLAY;info.configuration.setTo(process.context.getResources().getConfiguration());
        info.topActivityInfo=(ActivityInfo)HostUiAccess.get(last.record,ActivityThread.ActivityClientRecord.class,"activityInfo");
        info.taskDescription=new ActivityManager.TaskDescription(last.activity==null?null:String.valueOf(last.activity.getTitle()));
        info.isVisible=last.started;info.isVisibleRequested=!last.closing;info.isFocused=last.resumed;
        return info;
    }}
    static void excludeTask(int expected,boolean excluded){synchronized(activities){if(task.isEmpty()||taskId!=expected)throw new IllegalArgumentException("Activity task no longer exists");excludedFromRecents=excluded;}}
    static void finishTask(int expected){new Handler(Looper.getMainLooper()).post(()->{HostActivity top=top();if(top!=null&&currentTaskId()==expected)top.finish();});}
    static void resumeTask(int expected){new Handler(Looper.getMainLooper()).post(()->{if(currentTaskId()==expected)restoreTop();});}
    private Activity foreground(){HostActivity top=top();return top==null?activity:top.activity;}
    public static HostActivity owner(Activity activity){return state(activity);}
    /** Embedded platform settings own a real window, while the original
     * ActivityThread retains the caller's pause, result and resume sequence. */
    static AutoCloseable platformDialog(Activity activity){
        HostActivity state=state(activity);state.check();
        if(state.closed||state.closing)throw new IllegalStateException("Activity is closing");
        if(state.platformDialogs++==0&&state.resumed){
            var thread=HostApplication.current().thread;
            thread.handleTopResumedActivityChanged(state.record,false,"platform settings");
            thread.handlePauseActivity(state.record,false,false,false,new android.app.servertransaction.PendingTransactionActions(),"platform settings");
            state.resumed=false;
        }
        return new AutoCloseable(){boolean closed;@Override public void close(){
            state.check();if(closed)return;closed=true;
            if(--state.platformDialogs==0&&!state.closed&&!state.closing&&top()==state){
                restoreTop();
            }
        }};
    }
    public void drain(){check();HostLooper.drain(Looper.getMainLooper());}
    public void pump(){check();HostLooper.pump(Looper.getMainLooper());}
    public void resume(){HostActivity top=top();if(top!=null)resume(top.activity);}
    public void finish(){check();Throwable failure=null;while(top()!=null){HostActivity next=top();failure=attempt(failure,()->finish(next.activity));}if(failure!=null)HostUiAccess.raise(failure);}
    public void focus(boolean value){check();HostWindowManager.focusOutput(value);}
    /** Physical host sleep is independent of Android's virtual battery and
     * logical wake locks. Both VM bindings enter the same main-thread owner. */
    public static boolean hostSleeping(){return hostSleeping;}
    public static void hostSleep(boolean sleeping){
        if(!Looper.getMainLooper().isCurrentThread())throw new IllegalStateException("Activity sleep requires the main window thread");
        if(hostSleeping==sleeping)return;
        hostSleeping=sleeping;
        if(sleeping){
            HostWindowManager.hostSleepChanged();
            List<HostActivity> snapshot;synchronized(activities){snapshot=new ArrayList<>(task);}
            var process=HostApplication.current();
            for(var state:snapshot){
                if(state.closed||state.closing||!state.resumed)continue;
                state.resumeAfterSleep=true;
                process.thread.handleTopResumedActivityChanged(state.record,false,"host sleep");
                process.thread.handlePauseActivity(state.record,false,false,false,new android.app.servertransaction.PendingTransactionActions(),"host sleep");
                state.resumed=false;
            }
        }else{
            HostActivity state=top();
            if(state!=null&&!state.closed&&!state.closing&&state.resumeAfterSleep){
                state.resumeAfterSleep=false;
                if(state.launched)restoreTop();else resume(state.activity);
            }
            HostWindowManager.hostSleepChanged();
        }
    }
    public boolean closing(){synchronized(activities){return task.isEmpty()&&pendingLaunches==0;}}
    public long titleRevision(){check();String next=String.valueOf(foreground().getTitle());if(!next.equals(title)){title=next;titleRevision++;}return titleRevision;}
    public String title(){titleRevision();return title;}
    public boolean back(){HostActivity top=top();return top!=null&&!top.closed&&back(top.activity);}
    static IActivityClientController clientController(){return controller;}
    /** The native display supplies committed geometry; original AOSP records,
     * size buckets and transaction items own configuration and relaunch. */
    static void configurationChanged(Configuration config){
        var process=HostApplication.current();List<HostActivity> snapshot;
        synchronized(activities){snapshot=new ArrayList<>(task);}
        for(var state:snapshot){
            if(state.closed||state.closing||!state.created||state.activity==null)continue;
            Configuration old=(Configuration)HostUiAccess.get(state.activity,Activity.class,"mCurrentConfig");
            if(old==null||old.equals(config))continue;
            int changes=old.diff(config);
            var buckets=(android.window.SizeConfigurationBuckets)HostUiAccess.get(state.record,ActivityThread.ActivityClientRecord.class,"mSizeConfigurations");
            changes=android.window.SizeConfigurationBuckets.filterDiff(changes,old,config,buckets)&~ActivityInfo.CONFIG_WINDOW_CONFIGURATION;
            var info=(ActivityInfo)HostUiAccess.get(state.record,ActivityThread.ActivityClientRecord.class,"activityInfo");
            int handled=info.getRealConfigChanged();
            var bounds=config.windowConfiguration.getBounds();var window=new android.window.ActivityWindowInfo();window.set(false,bounds,bounds);
            var transaction=new android.app.servertransaction.ClientTransaction(process.thread.getApplicationThread());
            boolean relaunch=(changes&~handled)!=0&&(handled&ActivityInfo.CONFIG_RESOURCES_UNUSED)==0;
            int lifecycle=state.record.getLifecycleState();
            Activity previous=state.activity;boolean output=HostWindowManager.ownsOutput(previous);
            if(relaunch&&lifecycle>=2&&lifecycle<=5){
                int resize=ActivityInfo.CONFIG_SCREEN_SIZE|ActivityInfo.CONFIG_SMALLEST_SCREEN_SIZE|ActivityInfo.CONFIG_ORIENTATION|ActivityInfo.CONFIG_SCREEN_LAYOUT;
                transaction.addTransactionItem(new android.app.servertransaction.ActivityRelaunchItem(state.token,null,null,changes,
                    new android.util.MergedConfiguration(config,config),(changes&~resize)==0,window));
                transaction.addTransactionItem(android.app.servertransaction.TransactionExecutorHelper.getLifecycleRequestForCurrentState(state.record));
                process.thread.executeTransaction(transaction);
                state.activity=process.thread.getActivity(state.token);state.launched=true;
                if(output&&state.activity!=null)HostWindowManager.activate(state.activity);
            }else{
                transaction.addTransactionItem(new android.app.servertransaction.ActivityConfigurationChangeItem(state.token,config,window));
                process.thread.executeTransaction(transaction);
            }
        }
    }
    /** A declared fixed orientation applies before application code observes the
     * first display. Later orientation requests use the same original policy. */
    public static Boolean initialWindowLandscape(ActivityInfo info){
        int orientation=Objects.requireNonNull(info).screenOrientation;
        return ActivityInfo.isFixedOrientationLandscape(orientation)?Boolean.TRUE:
            ActivityInfo.isFixedOrientationPortrait(orientation)?Boolean.FALSE:null;
    }
    public static android.graphics.Point initialWindowSize(ActivityInfo info,int width,int height){
        Boolean landscape=initialWindowLandscape(info);
        return landscape!=null&&((landscape&&width<height)||(!landscape&&width>height))
            ?new android.graphics.Point(height,width):new android.graphics.Point(width,height);
    }
    static void install(){ActivityClient.setActivityClientController(controller);}
    /** Initial activities use the same original factory as ActivityThread's later launches. */
    public static Activity instantiate(ClassLoader loader,String name)throws ReflectiveOperationException{
        var process=HostApplication.current();Intent intent=new Intent(Intent.ACTION_MAIN)
            .setComponent(new ComponentName(process.context.getPackageName(),Objects.requireNonNull(name))).addCategory(Intent.CATEGORY_LAUNCHER);
        Objects.requireNonNull(loader);
        android.content.pm.ActivityInfo info;
        try{info=HostPackages.activityInfo(name);}catch(android.content.pm.PackageManager.NameNotFoundException failure){return HostUiAccess.raise(failure);}
        return process.instrumentation.newActivity(HostApplication.splitClassLoader(info.splitName),name,intent);
    }
    private static HostActivity state(Activity activity){synchronized(activities){for(HostActivity state:activities.values()){if(state.activity==null)state.activity=HostApplication.current().thread.getActivity(state.token);if(state.activity==activity)return state;}}throw new IllegalStateException("Activity is not attached to the native process");}
    private void check(){if(Thread.currentThread()!=owner)throw new IllegalStateException("Activity lifecycle requires its owner thread");}
    public static void attach(Activity activity,Context base,Application application){
        try{var component=new ComponentName(HostContext.hostPackageName(),activity.getClass().getName());attach(activity,base,application,base.getPackageManager().getActivityInfo(component,PackageManager.GET_META_DATA));}
        catch(PackageManager.NameNotFoundException failure){HostUiAccess.raise(failure);}
    }
    /** Factory substitutions retain the resolved Android component and original ActivityInfo. */
    public static void attach(Activity activity,Context base,Application application,ActivityInfo info){
        Objects.requireNonNull(info);
        if(!Objects.equals(HostContext.hostPackageName(),info.packageName)||info.applicationInfo==null||!HostPackages.owns(info))throw new SecurityException("Activity belongs to another application");
        HostApplication process=HostApplication.current();HostActivity state=new HostActivity(Objects.requireNonNull(activity));
        synchronized(activities){if(!activities.isEmpty())throw new IllegalStateException("The native window already has an initial Activity");register(state);}
        try{
            var component=new ComponentName(Objects.requireNonNull(info.packageName),Objects.requireNonNull(info.name));
            base=HostApplication.activityContext(info,state.token);
            Intent intent=new Intent(Intent.ACTION_MAIN).setComponent(component).addCategory(Intent.CATEGORY_LAUNCHER);
            Method attach=Arrays.stream(Activity.class.getDeclaredMethods()).filter(method->method.getName().equals("attach")&&method.getParameterCount()==20).findFirst().orElseThrow(()->new IllegalStateException("Android Activity attachment ABI changed"));attach.setAccessible(true);
            HostUiAccess.call(attach,activity,base,process.thread,process.instrumentation,state.token,0,application,intent,info,info.loadLabel(base.getPackageManager()),null,null,null,new Configuration(base.getResources().getConfiguration()),null,null,null,null,new Binder(),new Binder(),null);
            HostApplication.outerContext(base,activity);
            state.orientation=info.screenOrientation;
            state.record.token=state.token;state.record.packageInfo=process.apk;
            HostUiAccess.set(state.record,ActivityThread.ActivityClientRecord.class,"activity",activity);
            HostUiAccess.set(state.record,ActivityThread.ActivityClientRecord.class,"intent",intent);
            HostUiAccess.set(state.record,ActivityThread.ActivityClientRecord.class,"activityInfo",info);
            @SuppressWarnings("unchecked")var records=(Map<IBinder,ActivityThread.ActivityClientRecord>)HostUiAccess.get(process.thread,ActivityThread.class,"mActivities");records.put(state.token,state.record);
        }catch(Throwable failure){synchronized(activities){activities.remove(state.token);task.remove(state);}HostUiAccess.raise(failure);}
    }
    /** Local task-manager boundary. Original ActivityThread owns every app callback. */
    static void launch(ActivityInfo info,Intent original,IApplicationThread caller,IBinder resultTo,String who,int request,Bundle options){
        Intent intent=new Intent(original);
        Bundle windowOptions=options==null?null:new Bundle(options);
        // A completed finishing callback can exhaust the physical display turn
        // before its already accepted successor runs. The queued task request,
        // rather than the finished Activity, owns that exact handoff interval.
        synchronized(activities){pendingLaunches++;}
        boolean accepted=false;
        try{
            accepted=new Handler(Looper.getMainLooper()).post(()->{
                try{launchOnMain(info,intent,caller,resultTo,who,request,windowOptions);}
                finally{synchronized(activities){pendingLaunches--;}}
            });
            if(!accepted)throw new IllegalStateException("Android activity queue is closed");
        }finally{if(!accepted)synchronized(activities){pendingLaunches--;}}
    }
    static void launchUp(ActivityInfo info,Intent intent,IApplicationThread caller,IBinder parent){
        if(!Looper.getMainLooper().isCurrentThread())throw new IllegalStateException("Activity Up requires the task owner thread");
        launchOnMain(info,intent,caller,parent,null,-1,null);
    }
    /** Launcher-owned requests use Android's original result callback Binder.
     * The Intent and any authenticator response stay in the Android processes. */
    public static java.util.concurrent.CompletionStage<Void> request(Context context,Intent intent){
        var result=new java.util.concurrent.CompletableFuture<Void>();
        IIntentReceiver receiver=new IIntentReceiver.Stub(){
            @Override public void performReceive(Intent data,int code,String text,Bundle extras,boolean ordered,boolean sticky,int user){result.complete(null);}
        };
        try{
            int status=ActivityTaskManager.getService().startActivity(HostApplication.current().thread.getApplicationThread(),context.getPackageName(),null,
                new Intent(intent),intent.resolveTypeIfNeeded(context.getContentResolver()),receiver.asBinder(),null,0,0,null,null);
            Instrumentation.checkStartActivityResult(status,intent);
        }catch(Throwable failure){result.completeExceptionally(failure);}
        result.whenComplete((unused,failure)->{if(result.isCancelled())try{HostActivityTasks.cancelRequest(context,intent,receiver.asBinder());}catch(RemoteException ignored){}});
        return result;
    }
    static void finishChildren(IBinder token,String who,int request){
        new Handler(Looper.getMainLooper()).post(()->{
            List<HostActivity> snapshot;synchronized(activities){snapshot=new ArrayList<>(task);}
            Collections.reverse(snapshot);
            for(var state:snapshot)if(Objects.equals(state.resultTo,token)&&Objects.equals(state.resultWho,who)&&state.requestCode==request&&!state.closed)finish(state.activity);
            restoreTop();
        });
    }
    private static void launchOnMain(ActivityInfo info,Intent intent,IApplicationThread caller,IBinder resultTo,String who,int request,Bundle options){
        if(outputWindow!=null||maclauncher.android.bridge.WindowSession.attached()||HostWindowManager.outputAttached()){launchAttached(info,intent,caller,resultTo,who,request,false);return;}
        // A service process acquires the same output owner only when an actual
        // Activity needs presentation. It keeps its application and Binder
        // services after that output closes.
        var metrics=HostApplication.current().context.getResources().getDisplayMetrics();
        int width=Math.max(1,Math.min(800,(int)(metrics.widthPixels/metrics.density)));
        int height=Math.max(1,Math.min(600,(int)(metrics.heightPixels/metrics.density)));
        var viewport=initialWindowSize(info,width,height);
        var session=openWindow(String.valueOf(info.loadLabel(HostApplication.current().context.getPackageManager())),viewport.x,viewport.y,null,options);
        Throwable failure=null;
        try{
            launchAttached(info,intent,caller,resultTo,who,request,true);
            HostActivity state=top();
            if(state!=null&&state.activity!=null){
                session.attachActivity(state.activity);
                var display=HostDisplay.host();
                session.present(state.activity,String.valueOf(state.activity.getTitle()),display.getWidth(),display.getHeight());
            }
        }catch(Throwable error){failure=error;}
        finally{
            failure=closeWindow(failure,()->{HostActivity state=top();if(state!=null)state.finish();},session);
        }
        if(failure!=null)HostUiAccess.raise(failure);
    }
    private static void launchAttached(ActivityInfo info,Intent intent,IApplicationThread caller,IBinder resultTo,String who,int request,boolean firstOutput){
        HostApplication process=HostApplication.current();HostActivity previous=top();
        String className=info.targetActivity==null?info.name:info.targetActivity;
        boolean singleTop=info.launchMode!=ActivityInfo.LAUNCH_MULTIPLE||(intent.getFlags()&Intent.FLAG_ACTIVITY_SINGLE_TOP)!=0;
        if(singleTop&&previous!=null&&previous.activity.getClass().getName().equals(className)){
            process.thread.handleNewIntent(previous.record,List.of(new com.android.internal.content.ReferrerIntent(intent,process.context.getPackageName())));return;
        }
        var pending=new android.app.servertransaction.PendingTransactionActions();
        if(previous!=null&&previous.record.getLifecycleState()==3){process.thread.handlePauseActivity(previous.record,false,true,false,pending,"activity navigation");previous.resumed=false;}
        IBinder token=new Token();
        var record=new ActivityThread.ActivityClientRecord(token,intent,0,info,new Configuration(process.context.getResources().getConfiguration()),process.context.getPackageName(),null,null,null,null,null,null,true,null,process.thread,new Binder(),new Binder(),false,null,new Binder(),new android.window.ActivityWindowInfo());
        record.packageInfo=process.apk;
        HostActivity state=new HostActivity(record);state.orientation=info.screenOrientation;state.resultCaller=caller;state.resultTo=resultTo;state.resultWho=who;state.requestCode=request;
        register(state);
        try{
            state.activity=process.thread.handleLaunchActivity(record,pending,Context.DEVICE_ID_DEFAULT,null);state.created=true;
            if(state.activity==null){synchronized(activities){activities.remove(token);task.remove(state);}return;}
            if(!firstOutput&&!state.closing&&!state.activity.isFinishing()){
                process.thread.handleStartActivity(record,pending,null);state.started=true;HostActivityManager.hostStarted(state.activity);
                if(hostSleeping)state.resumeAfterSleep=true;
                else{process.thread.handleResumeActivity(record,true,true,false,"activity navigation");state.resumed=true;}
                HostWindowManager.activate(state.activity);
                if(!hostSleeping)process.thread.handleTopResumedActivityChanged(record,true,"activity navigation");
            }
            updateVisibility();
        }catch(Throwable failure){
            if(state.activity!=null)try{finish(state.activity);}catch(Throwable cleanup){failure.addSuppressed(cleanup);}
            else synchronized(activities){activities.remove(token);task.remove(state);}
            restoreTop();HostUiAccess.raise(failure);
        }
    }
    /** Original Window themes determine occlusion, independently of focus.
     * A translucent/result Activity pauses its predecessor but keeps that
     * predecessor's surface alive. An opaque Activity covers the whole visible
     * chain, which must restart together when that Activity goes away. */
    private static void updateVisibility(){
        List<HostActivity> snapshot;synchronized(activities){snapshot=new ArrayList<>(task);}
        Collections.reverse(snapshot);boolean covered=false;var process=HostApplication.current();
        for(var state:snapshot){
            if(state.activity==null||state.closed)continue;
            if(covered){
                if(state.started&&state.record.getLifecycleState()<5){var pending=new android.app.servertransaction.PendingTransactionActions();process.thread.handleStopActivity(state.record,pending,true,"activity occlusion");state.started=false;}
            }else{
                if(state.record.getLifecycleState()==5&&!state.closing){process.thread.performRestartActivity(state.record,true);state.started=true;}
                if(state.started&&!state.closing&&!state.activity.isFinishing()){
                    var attributes=state.activity.getWindow().getWindowStyle();
                    covered=!ActivityInfo.isTranslucentOrFloating(attributes)&&!attributes.getBoolean(com.android.internal.R.styleable.Window_windowNoDisplay,false);
                }
            }
        }
    }
    private static void restoreTop(){HostActivity state=top();if(state==null||state.closing||state.closed||state.platformDialogs>0)return;if(hostSleeping){state.resumeAfterSleep=true;return;}var process=HostApplication.current();updateVisibility();if(state.record.getLifecycleState()!=3)process.thread.handleResumeActivity(state.record,true,false,false,"activity return");state.started=true;state.resumed=true;HostWindowManager.activate(state.activity);process.thread.handleTopResumedActivityChanged(state.record,true,"activity return");}
    private void invoke(String method,Class<?>[] types,Object...arguments){HostUiAccess.call(HostUiAccess.method(Activity.class,method,types),activity,arguments);}
    public static void start(Activity activity,Bundle saved){HostActivity state=state(activity);state.check();if(state.created||state.closed)throw new IllegalStateException("Activity has already been created");state.created=true;state.invoke("performCreate",new Class<?>[]{Bundle.class},saved);state.record.setState(1);HostUiAccess.call(HostUiAccess.method(ActivityThread.class,"reportSizeConfigurations",ActivityThread.ActivityClientRecord.class),HostApplication.current().thread,state.record);}
    public static void resume(Activity activity){HostActivity state=state(activity);state.check();if(state.closed||!state.created)throw new IllegalStateException("Activity is not active");if(closing(activity)||state.platformDialogs>0)return;if(hostSleeping){state.resumeAfterSleep=true;return;}if(!state.started){state.started=true;state.invoke("performStart",new Class<?>[]{String.class},"native window");HostActivityManager.hostStarted(activity);state.record.setState(2);}if(!closing(activity)&&!state.resumed){state.resumed=true;state.invoke("performResume",new Class<?>[]{boolean.class,String.class},false,"native window");state.record.setState(3);}}
    public static void drain(Activity activity){state(activity).check();HostLooper.drain(Looper.getMainLooper());}
    public static boolean closing(Activity activity){return state(activity).closing||activity.isFinishing();}
    public static View content(Activity activity){return activity.getWindow().getDecorView();}
    /** Publish the native window attachment in the original process record. */
    static void windowAttached(Activity activity){HostActivity state=state(activity);state.check();HostUiAccess.set(state.record,ActivityThread.ActivityClientRecord.class,"window",activity.getWindow());HostUiAccess.set(activity,Activity.class,"mDecor",activity.getWindow().getDecorView());HostUiAccess.set(activity,Activity.class,"mWindowAdded",true);}
    /** Requests belong to Activity records; only the current output owner may
     * apply one. Activation replays the latest request after native ownership
     * changes, so a queued request cannot resize another Activity's window. */
    static void applyOrientation(Activity activity){
        HostActivity state=state(activity);state.check();
        if(state.closed||state.closing||!HostWindowManager.canOrientOutput(activity))return;
        int value=state.orientation;
        boolean landscape=ActivityInfo.isFixedOrientationLandscape(value);
        boolean portrait=ActivityInfo.isFixedOrientationPortrait(value);
        if((landscape||portrait)&&((HostDisplay.host().getWidth()>=HostDisplay.host().getHeight())!=landscape))HostWindowManager.requestOrientation(activity,landscape);
    }
    public static void focus(Activity activity,boolean focused){HostActivity state=state(activity);state.check();HostWindowManager.focusOutput(focused);}
    public static long titleRevision(Activity activity){HostActivity state=state(activity);state.check();String next=String.valueOf(activity.getTitle());if(next.length()>16384||next.indexOf('\0')>=0)throw new IllegalArgumentException("Activity title exceeds supported text bounds");if(!next.equals(state.title)){state.title=next;state.titleRevision++;}return state.titleRevision;}
    public static String title(Activity activity){titleRevision(activity);return state(activity).title;}
    public static boolean back(Activity activity){HostActivity state=state(activity);state.check();if(state.closed||closing(activity))return false;View root=content(activity);long now=SystemClock.uptimeMillis();root.dispatchKeyEvent(new KeyEvent(now,now,KeyEvent.ACTION_DOWN,KeyEvent.KEYCODE_BACK,0));root.dispatchKeyEvent(new KeyEvent(now,now,KeyEvent.ACTION_UP,KeyEvent.KEYCODE_BACK,0));return true;}
    private static Throwable keep(Throwable first,Throwable failure){if(first==null)return failure;if(first!=failure)first.addSuppressed(failure);return first;}
    private static Throwable attempt(Throwable first,Runnable action){try{action.run();}catch(Throwable failure){return keep(first,failure);}return first;}
    public static void finish(Activity activity){
        HostActivity state=state(activity);state.check();if(state.closed)return;state.closing=true;Throwable first=null;
        // Initial and navigated records share Android's finishing owner. It
        // marks mFinished before pause, then owns stop, destroy and ViewRoot
        // retirement while the native display and callback graph remain live.
        first=attempt(first,()->HostApplication.current().thread.handleDestroyActivity(state.record,true,false,"activity finish"));
        if(state.started)first=attempt(first,()->HostActivityManager.hostStopped(activity));
        state.closed=true;
        synchronized(activities){activities.remove(state.token);task.remove(state);}
        first=attempt(first,state::completeResult);
        first=attempt(first,HostActivity::taskEmpty);
        if(first!=null)HostUiAccess.raise(first);
    }
    private void completeResult(){
        try{deliverResult(resultCaller,resultTo,resultWho,requestCode,resultCode,resultData);}
        finally{resultCaller=null;resultTo=null;resultData=null;}
    }
    static void deliverResult(IApplicationThread caller,IBinder resultTo,String who,int request,int code,Intent data){
        if(resultTo==null||request<0)return;
        try{
            boolean local;synchronized(activities){local=activities.containsKey(resultTo);}
            if(local)HostApplication.current().thread.sendActivityResult(resultTo,who,request,code,data);
            else if("android.content.IIntentReceiver".equals(resultTo.getInterfaceDescriptor()))IIntentReceiver.Stub.asInterface(resultTo).performReceive(data,code,null,null,false,false,android.os.UserHandle.myUserId());
            else if(caller!=null){
                var transaction=new android.app.servertransaction.ClientTransaction(caller);
                transaction.addTransactionItem(new android.app.servertransaction.ActivityResultItem(resultTo,List.of(new ResultInfo(who,request,code,data))));
                RemoteException failure=transaction.schedule();if(failure!=null)System.err.println("[android-host] Activity result recipient disconnected: "+failure.getMessage());
            }
        }catch(RemoteException disconnected){System.err.println("[android-host] Activity result recipient disconnected");}
    }
    private static final class Controller extends IActivityClientController.Default {
        private HostActivity find(IBinder token){synchronized(activities){return activities.get(token);}}
        @Override public boolean shouldUpRecreateTask(IBinder token,String affinity){synchronized(activities){
            HostActivity source=activities.get(token);if(source==null||source.closed||task.isEmpty())return false;
            HostActivity root=task.getFirst();var info=(ActivityInfo)HostUiAccess.get(root.record,ActivityThread.ActivityClientRecord.class,"activityInfo");
            if(info.taskAffinity==null||!Objects.equals(info.taskAffinity,affinity))return true;
            Intent base=(Intent)HostUiAccess.get(root.record,ActivityThread.ActivityClientRecord.class,"intent");
            // Each native output owns one task, with no preceding document task.
            return source==root&&base!=null&&base.isDocument();
        }}
        @Override public boolean navigateUpTo(IBinder token,Intent destination,String type,int code,Intent data){
            Intent intent=new Intent(Objects.requireNonNull(destination)),result=data==null?null:new Intent(data);
            HostActivity source,parent;List<HostActivity> finish;int expected;boolean found=false;
            synchronized(activities){
                source=activities.get(token);if(source==null||source.closed||source.closing)return false;
                int index=task.indexOf(source);if(index<0)return false;
                int boundary=index-1;parent=boundary<0?null:task.get(boundary);
                ComponentName target=intent.getComponent();
                if(target!=null)for(int i=index-1;i>=0;i--)if(target.equals(componentForLocalToken(task.get(i).token))){parent=task.get(i);boundary=i;found=true;break;}
                finish=new ArrayList<>(task.subList(boundary+1,index+1));Collections.reverse(finish);expected=taskId;
            }
            if(found&&!HostActivityTasks.permitsUp(intent))return false;
            synchronized(activities){
                if(taskId!=expected||activities.get(token)!=source||source.closed||source.closing)return false;
                for(HostActivity state:finish)state.closing=true;
            }
            HostActivity target=parent;boolean matched=found;
            Handler queue=new Handler(Looper.getMainLooper());
            Runnable navigation=new Runnable(){int next;
                private void cancel(){synchronized(activities){for(HostActivity state:finish)if(activities.get(state.token)==state&&!state.closed)state.closing=false;}}
                @Override public void run(){
                synchronized(activities){if(taskId!=expected||next==0&&activities.get(token)!=source||target!=null&&!task.contains(target)){cancel();return;}}
                if(next<finish.size()){
                    HostActivity state=finish.get(next++);
                    if(!state.closed){state.resultCode=next==1?code:Activity.RESULT_CANCELED;state.resultData=next==1?result:null;finish(state.activity);}
                    // The original result transaction must reach its live
                    // recipient before that recipient's later destruction.
                    if(!queue.post(this))cancel();return;
                }
                if(matched&&!target.closed){
                    var process=HostApplication.current();
                    int status=HostActivityTasks.launchUp(intent,process.thread.getApplicationThread(),target.token);
                    // A normal launch replaces a standard parent; reusable
                    // launch modes retain it and deliver the original new Intent.
                    if(status==ActivityManager.START_SUCCESS&&top()!=target&&!target.closed){target.resultCode=Activity.RESULT_CANCELED;target.resultData=null;finish(target.activity);}
                }
                restoreTop();
                }
            };
            if(!queue.post(navigation)){synchronized(activities){for(HostActivity state:finish)if(!state.closed)state.closing=false;}return false;}
            return found;
        }
        @Override public void finishSubActivity(IBinder token,String who,int request){finishChildren(token,who,request);}
        @Override public boolean finishActivity(IBinder token,int code,Intent result,int mode){
            HostActivity state;boolean removeTask;int expectedTask;
            synchronized(activities){
                state=activities.get(token);if(state==null||state.closing)return false;
                state.closing=true;state.resultCode=code;state.resultData=result==null?null:new Intent(result);
                removeTask=mode==Activity.FINISH_TASK_WITH_ACTIVITY||
                    mode==Activity.FINISH_TASK_WITH_ROOT_ACTIVITY&&!task.isEmpty()&&task.getFirst()==state;
                expectedTask=taskId;
            }
            // Preserve the original server's root-only and always-remove modes.
            // The task owner rechecks identity when the main queue accepts it.
            if(removeTask)finishTask(expectedTask);
            else new Handler(Looper.getMainLooper()).post(()->{if(state.closed)return;finish(state.activity);restoreTop();});
            return true;
        }
        @Override public boolean finishActivityAffinity(IBinder token){return finishActivity(token,0,null,0);}
        @Override public boolean isTopOfTask(IBinder token){return find(token)==top();}
        @Override public boolean willActivityBeVisible(IBinder token){return find(token)==top();}
        @Override public int getDisplayId(IBinder token){return Display.DEFAULT_DISPLAY;}
        @Override public Configuration getTaskConfiguration(IBinder token){return new Configuration(HostApplication.current().context.getResources().getConfiguration());}
        @Override public ComponentName getCallingActivity(IBinder token){HostActivity state=find(token);return state==null?null:componentForToken(state.resultTo);}
        @Override public String getCallingPackage(IBinder token){ComponentName caller=getCallingActivity(token);return caller==null?null:caller.getPackageName();}
        @Override public int getTaskForActivity(IBinder token,boolean onlyRoot){synchronized(activities){var state=find(token);return state==null||(onlyRoot&&state!=task.getFirst())?-1:taskId;}}
        @Override public void setRequestedOrientation(IBinder token,int value){HostActivity state=find(token);if(state==null)return;if(value< -1||value>14)throw new IllegalArgumentException("Unknown orientation request");state.orientation=value;new Handler(Looper.getMainLooper()).post(()->{if(!state.closed&&!state.closing)applyOrientation(state.activity);});}
        @Override public int getRequestedOrientation(IBinder token){HostActivity state=find(token);return state==null?ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED:state.orientation;}
        @Override public void reportSizeConfigurations(IBinder token,android.window.SizeConfigurationBuckets buckets){HostActivity state=find(token);if(state!=null)HostUiAccess.set(state.record,ActivityThread.ActivityClientRecord.class,"mSizeConfigurations",buckets);}
    }
}
