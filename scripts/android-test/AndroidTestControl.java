package maclauncher.test;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.*;
import android.view.*;
import android.view.inputmethod.*;
import maclauncher.android.framework.*;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;

/** Shared Android test commands. VM instrumentation and diagnostics are injected. */
public final class AndroidTestControl {
    public interface Runtime {
        long pid();
        JSONObject platformState()throws JSONException;
        Object diagnostic(JSONObject request)throws Exception;
        String threads();
        default boolean closePresentation(){return false;}
    }
    private static Runtime runtime;
    public static void start(String token,boolean enabled,InputStream input,OutputStream output,Runtime backend){
        capability=token;diagnostics=enabled;runtime=Objects.requireNonNull(backend);
        if(enabled)ExitTrace.install();
        Thread server=new Thread(()->serve(input,output),"Android test control");server.setDaemon(true);server.start();
    }
    private static volatile Activity activity;
    private static volatile boolean resumed;
    private static volatile long lastActivityResumeAt;
    private static volatile boolean requestedFocus=true;
    private static volatile java.lang.ref.WeakReference<Object> presentation=new java.lang.ref.WeakReference<>(null);
    private static View focusView;
    // Original view notifications cover ART as well as instrumentation-based
    // JVM sessions. Mac window activation must not pause the background test
    // app or require an OS click before Android input can be investigated.
    private static final ViewTreeObserver.OnWindowFocusChangeListener focusChanged=focused->{
        if(activity!=null&&resumed&&presentation.get()!=null&&focused!=requestedFocus)queueOutputFocus();
    };
    private static String capability;
    private static boolean diagnostics;
    private static volatile long heartbeatPending,heartbeatCompleted,heartbeatLatency;
    private static final int INPUT_OBSERVATIONS=128;
    private static final Object inputObservationLock=new Object();
    private static final LinkedHashMap<Long,InputObservation> inputObservations=new LinkedHashMap<>();
    private static long nextInputTicket;
    /** Scalar observations only: Android retains and recycles every input event. */
    static final class InputObservation {
        final long ticket,createdAt=System.currentTimeMillis(),started=System.nanoTime();
        final JSONObject event;
        Boolean queued,handled;
        long completedAt,completed;
        String dispatchError;
        InputObservation(InputEvent value,int windowId)throws JSONException {
            event=json("sequence",value.getSequenceNumber(),"source",value.getSource(),"deviceId",value.getDeviceId(),"displayId",value.getDisplayId(),"requestedWindowId",windowId,"eventTimeMs",value.getEventTime());
            if(value instanceof MotionEvent motion){event.put("type","motion");event.put("action",motion.getActionMasked());event.put("x",motion.getX());event.put("y",motion.getY());}
            else if(value instanceof KeyEvent key){event.put("type","key");event.put("action",key.getAction());event.put("code",key.getKeyCode());}
            synchronized(inputObservationLock){ticket=++nextInputTicket;inputObservations.put(ticket,this);while(inputObservations.size()>INPUT_OBSERVATIONS)inputObservations.remove(inputObservations.keySet().iterator().next());}
        }
        void accepted(boolean value){synchronized(inputObservationLock){queued=value;}}
        void failed(Throwable error){synchronized(inputObservationLock){dispatchError=error.toString();}}
        void finished(boolean value){synchronized(inputObservationLock){if(completed!=0)return;handled=value;completed=System.nanoTime();completedAt=System.currentTimeMillis();}}
        JSONObject snapshot(){
            return json("ticket",ticket,"retained",true,"queued",queued==null?JSONObject.NULL:queued,"completed",completed!=0,"handled",handled==null?JSONObject.NULL:handled,"createdAt",createdAt,"completedAt",completed==0?JSONObject.NULL:completedAt,"completionLatencyMs",completed==0?JSONObject.NULL:(completed-started)/1_000_000.0,"pendingAgeMs",completed==0?(System.nanoTime()-started)/1_000_000.0:0,"event",event,"dispatchError",dispatchError==null?JSONObject.NULL:dispatchError);
        }
    }
    private static final class InputBatch {
        final ArrayDeque<Long> tickets=new ArrayDeque<>();
        int count;
        void add(long ticket){count++;tickets.addLast(ticket);if(tickets.size()>INPUT_OBSERVATIONS)tickets.removeFirst();}
        JSONObject result(JSONObject value)throws JSONException {
            value.put("inputEvents",count);value.put("inputTickets",new JSONArray(tickets));value.put("inputTicketsTruncated",count>tickets.size());
            synchronized(inputObservationLock){value.put("inputCompletions",inputSnapshots(tickets));}return value;
        }
    }
    private static JSONArray inputSnapshots(Collection<Long> tickets){
        JSONArray values=new JSONArray();for(long ticket:tickets){InputObservation value=inputObservations.get(ticket);values.put(value==null?json("ticket",ticket,"retained",false):value.snapshot());}return values;
    }
    private static JSONObject inputCompletions(JSONObject request)throws JSONException {
        synchronized(inputObservationLock){
            List<Long> tickets=new ArrayList<>();JSONArray selected=request.optJSONArray("tickets");
            if(selected==null)tickets.addAll(inputObservations.keySet());else{if(selected.length()>INPUT_OBSERVATIONS)throw new IllegalArgumentException("At most "+INPUT_OBSERVATIONS+" input tickets can be queried");for(int i=0;i<selected.length();i++)tickets.add(selected.getLong(i));}
            return json("capacity",INPUT_OBSERVATIONS,"retained",inputObservations.size(),"observations",inputSnapshots(tickets));
        }
    }
    static String inputOrigin(int sequence){
        synchronized(inputObservationLock){for(InputObservation value:inputObservations.values())if(value.event.optInt("sequence",-1)==sequence)return "android-test-hook";}
        return "outside-android-test-hook";
    }
    private static final Runnable heartbeat=()->{long now=System.nanoTime();heartbeatLatency=now-heartbeatPending;heartbeatCompleted=now;heartbeatPending=0;};
    private static JSONObject responsiveness()throws JSONException{
        Looper loop=Looper.getMainLooper();long now=System.nanoTime();
        JSONObject queue=json("available",loop!=null,"pendingAgeMs",heartbeatPending==0?0:(now-heartbeatPending)/1_000_000,
            "completed",heartbeatCompleted!=0,"lastRoundTripMs",heartbeatLatency/1_000_000);
        // At most one marker is outstanding. A hung UI must not accumulate
        // probes or block the independent diagnostic lane used to inspect it.
        if(loop!=null&&heartbeatPending==0){heartbeatPending=now;if(!new Handler(loop).post(heartbeat))heartbeatPending=0;}
        return json("ready",activity!=null,"androidQueue",queue,"cocoa",runtime.platformState().optJSONObject("cocoa"));
    }
    public static void attached(Activity value){activity=value;resumed=true;lastActivityResumeAt=System.currentTimeMillis();observeFocus(value);queueOutputFocus();}
    public static void paused(Activity value){if(activity==value){resumed=false;queueOutputFocus();}}
    public static void destroyed(Activity value){if(activity==value){resumed=false;activity=null;observeFocus(null);}}
    private static void observeFocus(Activity value){
        View next=value==null||value.getWindow()==null?null:value.getWindow().peekDecorView();
        if(next==focusView)return;
        if(focusView!=null)focusView.getViewTreeObserver().removeOnWindowFocusChangeListener(focusChanged);
        focusView=next;
        if(next!=null)next.getViewTreeObserver().addOnWindowFocusChangeListener(focusChanged);
    }
    /** Android can be the active test app without taking the Mac foreground. */
    public static boolean outputFocus(boolean nativeFocus){return activity!=null&&resumed&&presentation.get()!=null?requestedFocus:nativeFocus;}
    private static final Runnable applyOutputFocus=()->{if(activity!=null&&presentation.get()!=null)HostWindowManager.focusOutput(resumed&&requestedFocus);};
    private static void queueOutputFocus(){Looper loop=Looper.getMainLooper();if(loop!=null){Handler queue=new Handler(loop);queue.removeCallbacks(applyOutputFocus);queue.post(applyOutputFocus);}}
    public static void presentationAttached(Object value){presentation=new java.lang.ref.WeakReference<>(value);}
    public static void presentationDetached(Object value){if(presentation.get()==value){presentation.clear();observeFocus(null);}}
    private static JSONObject json(Object... values){try{JSONObject result=new JSONObject();for(int i=0;i<values.length;i+=2)result.put(String.valueOf(values[i]),values[i+1]);return result;}catch(JSONException error){throw new IllegalArgumentException(error);}}
    private static JSONObject platform(JSONObject result)throws JSONException{JSONObject state=runtime.platformState();for(String key:state.keySet())result.put(key,state.get(key));return result;}
    private static <T>T main(Callable<T> action)throws Exception {
        Looper loop=Looper.getMainLooper();if(loop==null)throw new IllegalStateException("Android main queue is not attached yet");
        if(loop.isCurrentThread())return action.call();
        CompletableFuture<T> result=new CompletableFuture<>();
        if(!new Handler(loop).post(()->{if(result.isCancelled())return;try{result.complete(action.call());}catch(Throwable error){result.completeExceptionally(error);}}))throw new IllegalStateException("Android window queue is closed");
        try{return result.get(10,TimeUnit.SECONDS);}catch(TimeoutException timeout){result.cancel(false);throw timeout;}
    }
    private static void serve(InputStream input,OutputStream output){
        ExecutorService actions=Executors.newSingleThreadExecutor(task->{Thread thread=new Thread(task,"Android test actions");thread.setDaemon(true);return thread;});
        ExecutorService probes=Executors.newSingleThreadExecutor(task->{Thread thread=new Thread(task,"Android test diagnostics");thread.setDaemon(true);return thread;});
        try(var read=new DataInputStream(input);var write=new DataOutputStream(output)){
            while(true){int count=read.readInt();if(count<1||count>16*1024*1024)throw new IOException("Invalid test control message length");byte[] bytes=read.readNBytes(count);if(bytes.length!=count)throw new EOFException();JSONObject request=new JSONObject(new String(bytes,StandardCharsets.UTF_8));
                // A blocked ViewRoot must not block the diagnostic capability
                // needed to explain it. Inputs retain one ordered action lane.
                boolean probe=Set.of("ping","responsiveness","threads","processes","sample","nativeMaps","memory","nativeAddresses","profileStatus","profileStop","inputCompletions").contains(request.optString("op"))||request.optString("op").equals("windowInputTrace")&&!request.optBoolean("start")&&!request.optBoolean("stop");
                (probe?probes:actions).execute(()->{
                    JSONObject response=json("id",request.optLong("id"),"pid",runtime.pid(),"at",System.currentTimeMillis());
                    try{if(!java.security.MessageDigest.isEqual(capability.getBytes(StandardCharsets.UTF_8),request.optString("capability").getBytes(StandardCharsets.UTF_8)))throw new SecurityException("Test session capability mismatch");response.put("result",command(request));}
                    catch(Throwable failure){Throwable cause=failure instanceof ExecutionException?failure.getCause():failure;try{response.put("error",cause.toString());}catch(JSONException ignored){}}
                    byte[] reply=response.toString().getBytes(StandardCharsets.UTF_8);
                    try{synchronized(write){write.writeInt(reply.length);write.write(reply);write.flush();}}catch(IOException disconnected){}
                });
            }
        }catch(EOFException expected){}catch(Throwable failure){System.err.println("[android-test] Control channel ended: "+failure);}
        finally{actions.shutdownNow();probes.shutdownNow();}
    }
    static Object command(JSONObject request)throws Exception {
        String operation=request.getString("op");
        if(operation.equals("ping")){Activity current=activity;return platform(json("ready",current!=null,"focused",current!=null&&current.hasWindowFocus(),"resumed",current!=null&&resumed,"lastActivityResumeAt",lastActivityResumeAt,"presentationReady",presentation.get()!=null,"diagnostics",diagnostics,"pid",runtime.pid()));}
        if(operation.equals("responsiveness"))return responsiveness();
        if(operation.equals("inputCompletions"))return inputCompletions(request);
        if(operation.equals("threads")){if(!diagnostics)throw new IllegalStateException("Diagnostics were disabled at launch");return runtime.threads();}
        if(operation.equals("processes")){if(!diagnostics)throw new IllegalStateException("Diagnostics were disabled at launch");return processes();}
        if(operation.equals("renderState")&&request.optBoolean("queue")){
            if(!diagnostics)throw new IllegalStateException("Diagnostics were disabled at launch");
            return main(()->RenderState.queued(request,window(request)));
        }
        if(Set.of("sample","nativeMaps","profileStatus","profileStop","inputTrace","memory","nativeAddresses","nativeSurfaces","renderState").contains(operation)){
            if(!diagnostics&&!operation.startsWith("profile"))throw new IllegalStateException("Diagnostics were disabled at launch");
            return runtime.diagnostic(request);
        }
        if(operation.equals("status"))return activity==null?unattachedStatus():main(()->status());
        if(operation.equals("views"))return main(()->views(request));
        if(operation.equals("windowInputTrace")){
            if(!diagnostics)throw new IllegalStateException("Diagnostics were disabled at launch");
            return request.optBoolean("start")||request.optBoolean("stop")?main(()->WindowInputTrace.control(request)):WindowInputTrace.snapshot();
        }
        if(operation.equals("objectState")){
            if(!diagnostics)throw new IllegalStateException("Diagnostics were disabled at launch");
            List<String> paths=objectPaths(request);
            return main(()->objectState(request,paths));
        }
        if(operation.equals("composition"))return main(()->composition());
        if(operation.equals("capture"))return capture(request);
        if(operation.equals("webScript")){
            if(!diagnostics)throw new IllegalStateException("Diagnostics were disabled at launch");
            CompletableFuture<String> result=new CompletableFuture<>();
            main(()->{android.webkit.WebView web=webView(window(request),request.optInt("viewId",0));if(web==null)throw new IllegalArgumentException("Selected Android window has no matching WebView");web.evaluateJavascript(request.getString("script"),result::complete);return null;});
            return json("result",result.get(10,TimeUnit.SECONDS));
        }
        if(operation.equals("wheel")){
            float horizontal=(float)request.optDouble("horizontal",0),vertical=(float)request.optDouble("vertical",0);
            if(!Float.isFinite(horizontal)||!Float.isFinite(vertical))throw new IllegalArgumentException("Wheel axes must be finite");
            float[] position=point(request,"");
            return main(()->{View view=window(request);int[] origin=new int[2];view.getLocationOnScreen(origin);HostInput.wheel(position[0]+origin[0],position[1]+origin[1],horizontal,vertical,request.optBoolean("allowTouch",true));return json("queued",true);});
        }
        if(operation.equals("click")||operation.equals("drag")||operation.equals("move"))return pointer(request);
        if(operation.equals("key")||operation.equals("keyDown")||operation.equals("keyUp")||operation.equals("back")){
            int code=operation.equals("back")?KeyEvent.KEYCODE_BACK:request.has("code")?request.getInt("code"):KeyEvent.keyCodeFromString("KEYCODE_"+request.getString("key").toUpperCase(Locale.ROOT));
            if(code==KeyEvent.KEYCODE_UNKNOWN)throw new IllegalArgumentException("Unknown Android key");int meta=request.optInt("meta",0);
            long downTime=SystemClock.uptimeMillis();
            InputBatch batch=new InputBatch();
            if(operation.equals("keyDown")||operation.equals("keyUp")){boolean queued=main(()->nativeInput(request,key(downTime,operation.equals("keyDown")?KeyEvent.ACTION_DOWN:KeyEvent.ACTION_UP,code,meta),batch));return batch.result(json("queued",queued,"code",code));}
            boolean down=main(()->nativeInput(request,key(downTime,KeyEvent.ACTION_DOWN,code,meta),batch));Thread.sleep(40);
            boolean up=main(()->nativeInput(request,key(downTime,KeyEvent.ACTION_UP,code,meta),batch));return batch.result(json("downQueued",down,"upQueued",up,"code",code));
        }
        if(operation.equals("text"))return main(()->{View focused=window(request).findFocus();String text=request.getString("text");if(focused==null)throw new IllegalStateException("No focused Android view");InputConnection connection=focused.onCreateInputConnection(new EditorInfo());if(connection!=null)return json("committed",connection.commitText(text,1),"route","InputConnection");return json("queued",dispatch(request,new KeyEvent(SystemClock.uptimeMillis(),text,-1,0)),"route","KeyEvent");});
        if(operation.equals("focus")){
            boolean focused=request.optBoolean("focused",true);
            main(()->{if(presentation.get()==null)throw new IllegalStateException("This process does not own an Android presentation");requestedFocus=focused;HostWindowManager.focusOutput(focused);return null;});
            // ViewRoot posts its original focus message. A second queue turn
            // observes it before another command can overwrite pending focus.
            return main(()->json("focused",focused,"observed",activity!=null&&activity.hasWindowFocus()));
        }
        if(operation.equals("close"))return main(()->{Activity current=activity;if(current!=null)closeTask(current);else if(presentation.get()==null||!runtime.closePresentation())throw new IllegalStateException("This process has no Android activity or presentation to close");return json("requested",true);});
        throw new IllegalArgumentException("Unknown Android test operation: "+operation);
    }
    /** A session close removes its exact owned task, including nested Activities.
     * Activity.finishAndRemoveTask only removes the task when called by its root. */
    private static void closeTask(Activity current){
        int id=current.getTaskId(),uid=current.getApplicationInfo().uid;
        String packageName=current.getPackageName();
        if(id>=0&&uid==android.os.Process.myUid()&&packageName.equals(HostContext.hostPackageName())){
            var manager=current.getSystemService(android.app.ActivityManager.class);
            if(manager!=null)for(var task:manager.getAppTasks()){
                var info=task.getTaskInfo();
                if(info!=null&&info.taskId==id&&info.effectiveUid==uid&&info.userId==current.getUserId()&&
                    info.baseActivity!=null&&packageName.equals(info.baseActivity.getPackageName())){
                    task.finishAndRemoveTask();return;
                }
            }
        }
        throw new IllegalStateException("The current Android activity has no matching owned task");
    }
    private static Object field(Object owner,String name)throws Exception{var field=owner.getClass().getDeclaredField(name);field.setAccessible(true);return field.get(owner);}
    private static JSONObject processes()throws Exception{
        var holder=HostRemoteServices.class.getDeclaredField("processes");holder.setAccessible(true);Object owner=holder.get(null);var result=new JSONArray();
        synchronized(owner){for(var entry:((Map<?,?>)field(owner,"processes")).entrySet()){
            Object identity=entry.getKey();var key=(HostProcessServices.Key)(identity instanceof HostProcessServices.Key?identity:field(identity,"key"));Object state=entry.getValue();var expected=(HostBinderRpc.Peer)field(state,"expectedPeer");
            result.put(json("packageName",key.packageName(),"processName",key.processName(),"expectedPid",expected==null?null:expected.pid(),
                "connection",field(state,"connection")!=null,"directoryReady",((CompletableFuture<?>)field(state,"directory")).isDone(),"retired",field(state,"retired")));
        }}
        return json("pid",runtime.pid(),"processName",HostContext.hostProcessName(),"packageName",HostContext.hostApplicationInfoSnapshot().packageName,"peers",result);
    }
    private static View window(JSONObject request)throws JSONException {
        if(!request.has("windowId")){
            Activity current=activity;if(current==null)throw new IllegalStateException("Android window is not attached yet");
            View decor=current.getWindow().getDecorView();
            // Dialogs and popups are original WindowManagerGlobal roots too.
            for(View view:WindowManagerGlobal.getInstance().getWindowViews())if(view.hasWindowFocus()&&view.getDisplay()!=null&&decor.getDisplay()!=null&&view.getDisplay().getDisplayId()==decor.getDisplay().getDisplayId())return view;
            return decor;
        }
        int id=request.getInt("windowId");for(View view:WindowManagerGlobal.getInstance().getWindowViews())if(System.identityHashCode(view)==id)return view;
        throw new IllegalArgumentException("Android window is no longer owned: "+id);
    }
    private static android.webkit.WebView webView(View view,int id){
        if(view instanceof android.webkit.WebView web&&(id==0||System.identityHashCode(view)==id))return web;
        if(view instanceof ViewGroup group)for(int index=0;index<group.getChildCount();index++){var found=webView(group.getChildAt(index),id);if(found!=null)return found;}
        return null;
    }
    private static List<String> objectPaths(JSONObject request)throws JSONException {
        JSONArray values=request.getJSONArray("paths");
        if(values.length()<1||values.length()>32)throw new IllegalArgumentException("Select 1–32 explicit field paths");
        List<String> paths=new ArrayList<>();
        for(int i=0;i<values.length();i++){
            String path=values.getString(i);String[] fields=path.split("\\.",-1);
            if(path.length()>256||fields.length>16)throw new IllegalArgumentException("Field paths are limited to 256 characters and 16 fields");
            for(String name:fields)if(!name.matches("[A-Za-z_$][A-Za-z0-9_$]*"))throw new IllegalArgumentException("Field paths contain only dot-separated field names");
            paths.add(path);
        }
        return paths;
    }
    private static View stateView(JSONObject request)throws JSONException {
        View root=window(request);if(!request.has("viewId"))return root;
        int id=request.getInt("viewId"),visited=0;ArrayDeque<View> pending=new ArrayDeque<>();pending.add(root);
        while(!pending.isEmpty()){
            if(++visited>4096)throw new IllegalStateException("Selected window exceeds the bounded view search");
            View view=pending.removeFirst();if(System.identityHashCode(view)==id)return view;
            if(view instanceof ViewGroup group)for(int i=0;i<group.getChildCount();i++){if(visited+pending.size()>=4096)throw new IllegalStateException("Selected window exceeds the bounded view search");pending.addLast(group.getChildAt(i));}
        }
        throw new IllegalArgumentException("Selected Android view is no longer owned: "+id);
    }
    private static JSONObject objectIdentity(Object value){return value==null?json("kind","null"):json("kind","reference","class",value.getClass().getName(),"identity",System.identityHashCode(value));}
    private static JSONObject fieldState(Object value,Class<?> type){
        if(value==null)return json("kind","null","type",type.getName());
        Class<?> actual=value.getClass();Object scalar;
        if(actual==Boolean.class||actual==Byte.class||actual==Short.class||actual==Integer.class)scalar=value;
        else if(actual==java.util.concurrent.atomic.AtomicBoolean.class)scalar=((java.util.concurrent.atomic.AtomicBoolean)value).get();
        else if(actual==java.util.concurrent.atomic.AtomicInteger.class)scalar=((java.util.concurrent.atomic.AtomicInteger)value).get();
        else if(actual==java.util.concurrent.atomic.AtomicLong.class)return json("kind","scalar","type",type.getName(),"value",Long.toString(((java.util.concurrent.atomic.AtomicLong)value).get()),"encoding","decimal-integer");
        else if(actual==Character.class)return json("kind","scalar","type",type.getName(),"value",(int)(Character)value,"encoding","character-code");
        else if(actual==Long.class)return json("kind","scalar","type",type.getName(),"value",Long.toString((Long)value),"encoding","decimal-integer");
        else if(actual==Float.class){float number=(Float)value;scalar=Float.isFinite(number)?number:Float.isNaN(number)?"NaN":number>0?"Infinity":"-Infinity";}
        else if(actual==Double.class){double number=(Double)value;scalar=Double.isFinite(number)?number:Double.isNaN(number)?"NaN":number>0?"Infinity":"-Infinity";}
        else return objectIdentity(value);
        return json("kind","scalar","type",type.getName(),"value",scalar);
    }
    private static JSONObject objectState(JSONObject request,List<String> paths)throws JSONException {
        String selection=request.optString("target","activity");Object root;
        if(selection.equals("activity")){root=activity;if(root==null)throw new IllegalStateException("Android Activity is not attached yet");}
        else if(selection.equals("view"))root=stateView(request);
        else throw new IllegalArgumentException("Object target must be activity or view");
        JSONArray results=new JSONArray();
        for(String path:paths){
            JSONObject row=json("path",path);Object value=root;String segment=null;
            try{
                for(String name:path.split("\\.")){
                    segment=name;if(value==null)throw new NullPointerException();
                    java.lang.reflect.Field member=null;
                    for(Class<?> owner=value.getClass();owner!=null;owner=owner.getSuperclass())try{member=owner.getDeclaredField(name);break;}catch(NoSuchFieldException missing){}
                    if(member==null)throw new NoSuchFieldException();
                    member.setAccessible(true);value=member.get(value);
                    row.put("declaringClass",member.getDeclaringClass().getName());row.put("fieldType",member.getType().getName());
                    row.put("state",fieldState(value,member.getType()));
                }
            }catch(ReflectiveOperationException|RuntimeException failure){row.remove("state");row.put("error",json("class",failure.getClass().getName(),"field",segment));}
            results.put(row);
        }
        // This is a bounded field read on the UI owner, not an app-state lock
        // or an atomic snapshot of fields concurrently updated by its workers.
        return json("target",selection,"object",objectIdentity(root),"at",System.currentTimeMillis(),"concurrentSnapshot",true,"fields",results);
    }
    private static boolean dispatch(JSONObject request,InputEvent event)throws JSONException {
        try{View view=window(request);ViewRootImpl root=view.getViewRootImpl();
            if(root==null)throw new IllegalStateException("Selected Android window is detached");
            if(view.getDisplay()!=null)event.setDisplayId(view.getDisplay().getDisplayId());
            root.dispatchInputEvent(event);return true;
        }catch(RuntimeException|JSONException failure){event.recycle();throw failure;}
    }
    private static KeyEvent key(long downTime,int action,int code,int meta){return new KeyEvent(downTime,SystemClock.uptimeMillis(),action,code,0,meta,KeyCharacterMap.VIRTUAL_KEYBOARD,0,0,InputDevice.SOURCE_KEYBOARD);}
    /** The same native-device ingress, including host controls and pointer ownership. */
    private static boolean nativeInput(JSONObject request,InputEvent event,InputBatch batch)throws JSONException {
        View view;try{view=window(request);if(view.getViewRootImpl()==null)throw new IllegalStateException("Selected Android window is detached");
        }catch(RuntimeException|JSONException failure){event.recycle();throw failure;}
        InputObservation observation=new InputObservation(event,System.identityHashCode(view));batch.add(observation.ticket);
        return observedInput(event,observation);
    }
    static boolean observedInput(InputEvent event,InputObservation observation){
        // The callback deliberately captures no InputEvent. The original
        // ViewRoot/InputChannel pipeline remains its sole lifetime owner.
        // Only the numeric ticket survives asynchronous dispatch; evicting
        // history must also release its scalar observation while input waits.
        long ticket=observation.ticket;
        try{boolean accepted=HostWindowManager.input(event,handled->observedInputFinished(ticket,handled));observation.accepted(accepted);return accepted;}
        catch(RuntimeException|Error failure){observation.failed(failure);throw failure;}
    }
    private static void observedInputFinished(long ticket,boolean handled){synchronized(inputObservationLock){InputObservation observation=inputObservations.get(ticket);if(observation!=null)observation.finished(handled);}}
    private static JSONObject unattachedStatus(){return json("ready",false,"presentationReady",presentation.get()!=null,"resumed",false,"lastActivityResumeAt",lastActivityResumeAt,"width",0,"height",0,"windows",new JSONArray());}
    private static JSONObject status()throws Exception{
        if(activity==null)return unattachedStatus();
        View decor=window(new JSONObject());var metrics=decor.getResources().getDisplayMetrics();JSONArray windows=new JSONArray();
        for(View view:WindowManagerGlobal.getInstance().getWindowViews()){List<SurfaceView> sources=new ArrayList<>();surfaces(view,sources);ViewRootImpl root=view.getViewRootImpl();windows.put(json("id",System.identityHashCode(view),"class",view.getClass().getName(),"title",root==null?"":String.valueOf(root.mWindowAttributes.getTitle()),"width",view.getWidth(),"height",view.getHeight(),"visible",view.getVisibility()==View.VISIBLE,"focused",view.hasWindowFocus(),"surfaceViews",sources.size(),"canvasSurface",root!=null&&root.mSurface.isValid()));}
        return platform(json("ready",true,"activity",activity.getClass().getName(),"package",activity.getPackageName(),"width",decor.getWidth(),"height",decor.getHeight(),"density",metrics.density,"densityDpi",metrics.densityDpi,"coordinateSpace","Android window pixels; normalized coordinates map to this extent","focused",decor.hasWindowFocus(),"resumed",resumed,"lastActivityResumeAt",lastActivityResumeAt,"windows",windows));
    }
    private static float[] point(JSONObject request,String prefix)throws Exception {
        double x=request.getDouble(prefix+"x"),y=request.getDouble(prefix+"y");if(!Double.isFinite(x)||!Double.isFinite(y))throw new IllegalArgumentException("Coordinates must be finite");
        return main(()->{View decor=window(request);if(decor.getWidth()<1||decor.getHeight()<1)throw new IllegalStateException("Selected Android window has no extent");if(request.optString("space","normalized").equals("normalized")){if(x<0||x>1||y<0||y>1)throw new IllegalArgumentException("Normalized coordinates must be between 0 and 1");return new float[]{(float)(x*Math.max(0,decor.getWidth()-1)),(float)(y*Math.max(0,decor.getHeight()-1))};}if(!request.optString("space").equals("pixels"))throw new IllegalArgumentException("Coordinate space must be normalized or pixels");return new float[]{(float)x,(float)y};});
    }
    private static boolean touch(JSONObject request,long downTime,int action,float[] p,InputBatch batch)throws Exception{return main(()->{View view=window(request);int[] origin=new int[2];view.getLocationOnScreen(origin);return nativeInput(request,HostInput.motion(action,p[0]+origin[0],p[1]+origin[1]),batch);});}
    private static Object pointer(JSONObject request)throws Exception {
        // Keep the original DOWN target for the entire gesture even if that
        // input opens a dialog or another Activity changes the focused window.
        JSONObject target=new JSONObject(request.toString());if(!target.has("windowId"))target.put("windowId",main(()->System.identityHashCode(window(request))));
        InputBatch batch=new InputBatch();float[] start=point(target,"");long downTime=SystemClock.uptimeMillis();String op=target.getString("op");if(op.equals("move"))return batch.result(json("queued",touch(target,downTime,MotionEvent.ACTION_MOVE,start,batch)));
        boolean down=touch(target,downTime,MotionEvent.ACTION_DOWN,start,batch),up=false;
        try{if(op.equals("drag")){float[] end=point(target,"to");long duration=target.optLong("durationMs",500);if(duration<1||duration>30000)throw new IllegalArgumentException("Drag duration must be 1–30000ms");long begin=System.nanoTime();int steps=(int)Math.max(1,Math.ceil(duration/16.0));for(int i=1;i<=steps;i++){long remaining=begin+duration*1_000_000L*i/steps-System.nanoTime();if(remaining>0)TimeUnit.NANOSECONDS.sleep(remaining);float t=(float)i/steps;touch(target,downTime,MotionEvent.ACTION_MOVE,new float[]{start[0]+(end[0]-start[0])*t,start[1]+(end[1]-start[1])*t},batch);}up=touch(target,downTime,MotionEvent.ACTION_UP,end,batch);}else{Thread.sleep(60);up=touch(target,downTime,MotionEvent.ACTION_UP,start,batch);}}
        finally{if(!up)touch(target,downTime,MotionEvent.ACTION_CANCEL,start,batch);}return batch.result(json("downQueued",down,"upQueued",up,"windowId",target.opt("windowId")));
    }
    private static void surfaces(View view,List<SurfaceView> out){if(view instanceof SurfaceView surface&&surface.getHolder().getSurface().isValid())out.add(surface);if(view instanceof ViewGroup group)for(int i=0;i<group.getChildCount();i++)surfaces(group.getChildAt(i),out);}
    private static JSONObject drawable(android.graphics.drawable.Drawable drawable)throws JSONException{
        if(drawable==null)return null;
        var bounds=drawable.getBounds();
        JSONObject result=json("class",drawable.getClass().getName(),"intrinsicWidth",drawable.getIntrinsicWidth(),"intrinsicHeight",drawable.getIntrinsicHeight(),"left",bounds.left,"top",bounds.top,"right",bounds.right,"bottom",bounds.bottom);
        if(drawable instanceof android.graphics.drawable.BitmapDrawable bitmap){Bitmap value=bitmap.getBitmap();if(value!=null)result.put("bitmap",json("width",value.getWidth(),"height",value.getHeight(),"density",value.getDensity(),"filtered",bitmap.getPaint().isFilterBitmap()));}
        return result;
    }
    private static JSONObject viewTree(View view)throws JSONException{
        int[] position=new int[2];view.getLocationInWindow(position);
        JSONObject node=json("id",System.identityHashCode(view),"class",view.getClass().getName(),"x",position[0],"y",position[1],"width",view.getWidth(),"height",view.getHeight(),"shown",view.isShown(),"alpha",view.getAlpha(),"enabled",view.isEnabled(),"clickable",view.isClickable(),"focused",view.isFocused());
        if(view.getId()!=View.NO_ID)try{node.put("resource",view.getResources().getResourceName(view.getId()));}catch(android.content.res.Resources.NotFoundException ignored){node.put("resourceId",view.getId());}
        if(view instanceof android.widget.TextView text)node.put("text",String.valueOf(text.getText()));
        if(view instanceof android.widget.ImageView image){float[] matrix=new float[9];image.getImageMatrix().getValues(matrix);JSONArray values=new JSONArray();for(float value:matrix)values.put(value);node.put("image",json("scaleType",image.getScaleType().name(),"matrix",values,"drawable",drawable(image.getDrawable())));}
        if(view.getBackground()!=null)node.put("background",drawable(view.getBackground()));
        if(view.getContentDescription()!=null)node.put("description",String.valueOf(view.getContentDescription()));
        if(view instanceof SurfaceView surface){android.graphics.Rect frame=surface.getHolder().getSurfaceFrame();node.put("surface",json("valid",surface.getHolder().getSurface().isValid(),"width",frame.width(),"height",frame.height()));}
        if(view instanceof ViewGroup group){JSONArray children=new JSONArray();for(int i=0;i<group.getChildCount();i++)children.put(viewTree(group.getChildAt(i)));node.put("children",children);}
        return node;
    }
    private static JSONObject views(JSONObject request)throws JSONException{
        JSONArray windows=new JSONArray();
        for(View view:WindowManagerGlobal.getInstance().getWindowViews())if(!request.has("windowId")||System.identityHashCode(view)==request.getInt("windowId"))windows.put(viewTree(view));
        return json("activity",activity.getClass().getName(),"coordinateSpace","Android window pixels","windows",windows);
    }
    private static JSONObject surfaceControl(SurfaceControl control,View view)throws JSONException{
        if(control==null)return null;
        boolean valid=control.isValid();int[] location=new int[2];view.getLocationInWindow(location);
        return json("id",valid?control.getLayerId():null,"name",control.toString(),"closed",!valid,"viewId",System.identityHashCode(view),"x",location[0],"y",location[1],"width",view.getWidth(),"height",view.getHeight());
    }
    private static JSONObject composition()throws Exception{
        View selected=window(new JSONObject());JSONObject selectedRoot=null;JSONArray windows=new JSONArray();
        for(View view:WindowManagerGlobal.getInstance().getWindowViews()){
            ViewRootImpl root=view.getViewRootImpl();if(root==null)continue;
            JSONObject node=surfaceControl(root.getSurfaceControl(),view);if(node==null)continue;
            JSONArray children=new JSONArray();List<SurfaceView> sources=new ArrayList<>();surfaces(view,sources);
            for(SurfaceView source:sources){JSONObject child=surfaceControl(source.getSurfaceControl(),source);if(child!=null)children.put(child);}
            node.put("children",children);windows.put(node);if(view==selected)selectedRoot=node;
        }
        // These are actual client-owned SurfaceControls. SurfaceFlinger owns
        // their global ordering and buffers; this is not a second scene graph.
        return json("attached",selectedRoot!=null,"scope","owned-window-surfaces","root",selectedRoot,"windows",windows);
    }
    private record CapturedFrame(Bitmap bitmap,int width,int height,String source,int windowId,boolean visible){}
    private static Object capture(JSONObject request)throws Exception {
        CompletableFuture<CapturedFrame> result=new CompletableFuture<>();
        CapturedFrame frame;
        try{
        String selection=request.optString("source","auto");if(!Set.of("auto","canvas","surface").contains(selection))throw new IllegalArgumentException("Capture source must be auto, canvas or surface");
        main(()->{View decor=window(request);List<SurfaceView> surfaces=new ArrayList<>();surfaces(decor,surfaces);SurfaceView surface=selection.equals("canvas")?null:surfaces.stream().filter(v->!request.has("surfaceId")||System.identityHashCode(v)==request.optInt("surfaceId")).max(Comparator.comparingLong(v->(long)v.getWidth()*v.getHeight())).orElse(null);
            if(surface==null&&(selection.equals("surface")||request.has("surfaceId")))throw new IllegalArgumentException("Selected Android window has no matching SurfaceView");
            int width=surface==null?decor.getWidth():surface.getWidth(),height=surface==null?decor.getHeight():surface.getHeight();if(width<1||height<1)throw new IllegalStateException("Android surface has no extent");
            int maximum=request.optInt("maxWidth",0);if(maximum>0&&width>maximum){height=Math.max(1,(int)Math.round((double)height*maximum/width));width=maximum;}
            Bitmap bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);String source=surface==null?"android-window-canvas":"android-surface-producer-before-effects";
            CapturedFrame copied=new CapturedFrame(bitmap,width,height,source,System.identityHashCode(decor),decor.getVisibility()==View.VISIBLE);
            PixelCopy.OnPixelCopyFinishedListener callback=status->{if(status==PixelCopy.SUCCESS){if(!result.complete(copied))bitmap.recycle();}else{bitmap.recycle();result.completeExceptionally(new IllegalStateException("PixelCopy returned "+status));}};
            try{if(surface==null){ViewRootImpl root=decor.getViewRootImpl();if(root==null||!root.mSurface.isValid())throw new IllegalStateException("Selected Android window has no valid Canvas surface");PixelCopy.request(root.mSurface,bitmap,callback,new Handler(Looper.getMainLooper()));}else PixelCopy.request(surface,bitmap,callback,new Handler(Looper.getMainLooper()));}catch(Throwable failure){bitmap.recycle();throw failure;}return null;
        });
        frame=result.get(15,TimeUnit.SECONDS);}catch(Exception failure){
            if(!result.cancel(false)){CapturedFrame abandoned=result.handle((value,error)->value).join();if(abandoned!=null)abandoned.bitmap.recycle();}throw failure;
        }
        // Encoding is CPU-heavy; keep it on the requesting test worker, never
        // the Android UI/frame thread being measured. PixelCopy owns the bitmap
        // until completion, then this worker owns it through PNG serialization.
        try{ByteArrayOutputStream png=new ByteArrayOutputStream();if(!frame.bitmap.compress(Bitmap.CompressFormat.PNG,100,png))throw new IOException("PNG compression failed");return json("width",frame.width,"height",frame.height,"source",frame.source,"windowId",frame.windowId,"windowVisible",frame.visible,"png",Base64.getEncoder().encodeToString(png.toByteArray()));}finally{frame.bitmap.recycle();}
    }
}
