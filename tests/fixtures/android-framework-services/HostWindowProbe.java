package maclauncher.android;

import android.app.*;
import android.content.*;
import android.content.pm.*;
import android.os.*;
import android.view.*;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.PixelFormat;
import java.nio.file.*;
import java.util.*;
import maclauncher.android.framework.*;

/** Original Activity/PhoneWindow/ViewRoot behavior over the actual local platform owners. */
public final class HostWindowProbe {
    private static int checks,frames;private static final List<String> events=new ArrayList<>();
    private static long sequence;private static int[] latestPixels;
    static SurfaceControl fixtureOutput;static int outputWidth=320,outputHeight=240;
    static void pump(Looper loop){HostLooper.drain(loop);long now=System.nanoTime();HostDisplayEvents.hostDispatchFrame(++sequence,now,now+16_666_667,16_666_667,1);HostDisplayEvents.hostPresentFrame();HostLooper.drain(loop);}
    private static void check(boolean value,String text){checks++;if(!value)throw new AssertionError(text);}
    private static void retirePreferences()throws Exception{
        android.app.QueuedWork.waitToFinish();android.app.QueuedWork.resetHandler();
        var executor=Class.forName("android.app.SharedPreferencesImpl").getDeclaredField("sLoadExecutor");executor.setAccessible(true);
        ((java.util.concurrent.ExecutorService)executor.get(null)).shutdown();
    }
    private static void textInput(OwnedActivity activity,Looper loop)throws Exception{
        System.load(System.getProperty("klepton.graphics"));
        View previous=activity.content;
        class CopyEditor extends android.widget.EditText {
            String selectedForExport;
            CopyEditor(){super(activity);}
            @Override public android.view.inputmethod.InputConnection onCreateInputConnection(android.view.inputmethod.EditorInfo info){
                return new android.view.inputmethod.InputConnectionWrapper(super.onCreateInputConnection(info),false){
                    @Override public CharSequence getSelectedText(int flags){CharSequence value=super.getSelectedText(flags);selectedForExport=value==null?null:value.toString();return value;}
                    @Override public boolean performContextMenuAction(int id){boolean result=super.performContextMenuAction(id);if(id==android.R.id.copy)CopyEditor.this.setSelection(getSelectionEnd());return result;}
                };
            }
        }
        var editor=new CopyEditor();editor.setSingleLine(true);
        activity.setContentView(editor);check(editor.requestFocus(),"original EditText accepts focus");
        long deadline=System.nanoTime()+2_000_000_000L;
        do{pump(loop);Thread.sleep(1);}while(!HostTextInput.active()&&System.nanoTime()<deadline);
        check(HostTextInput.active(),"original InputMethodManager publishes its focused connection");
        HostTextInput.text("a",false,0,0);pump(loop);
        check(editor.getText().toString().equals("a"),"one text commit produces one character");
        HostTextInput.text("é🎮",false,0,0);pump(loop);
        check(editor.getText().toString().equals("aé🎮"),"committed Unicode reaches the original editable");
        HostTextInput.text("ni",true,0,2);pump(loop);
        check(editor.getSelectionStart()==4&&editor.getSelectionEnd()==6,"composition selection uses Android UTF-16 offsets");
        HostTextInput.text("你",false,0,0);pump(loop);
        check(editor.getText().toString().equals("aé🎮你"),"composition is replaced by its final commit");
        check(android.view.inputmethod.BaseInputConnection.getComposingSpanStart(editor.getText())==-1,"commit retires the composing region");
        HostTextInput.shortcut(KeyEvent.KEYCODE_A);pump(loop);
        check(editor.getSelectionStart()==0&&editor.getSelectionEnd()==editor.length(),"Cmd-A uses the original editor selection");
        HostTextInput.shortcut(KeyEvent.KEYCODE_C);pump(loop);
        var clipboard=activity.getSystemService(android.content.ClipboardManager.class);
        check(clipboard.getPrimaryClip().getItemAt(0).getText().toString().equals("aé🎮你"),"Cmd-C retains the original Android clipboard copy");
        check("aé🎮你".equals(editor.selectedForExport)&&editor.getSelectionStart()==editor.getSelectionEnd(),"host copy captures selection before an editor finishes its selection action mode");
        HostTextInput.shortcut(KeyEvent.KEYCODE_A);pump(loop);
        HostTextInput.shortcut(KeyEvent.KEYCODE_X);pump(loop);
        check(editor.length()==0&&clipboard.getPrimaryClip().getItemAt(0).getText().toString().equals("aé🎮你"),"Cmd-X cuts through the private Android clipboard");
        HostTextInput.text("replacement",false,0,0);pump(loop);
        check(editor.getText().toString().equals("replacement"),"commit after cut inserts text once");
        var other=new android.widget.EditText(activity);activity.setContentView(other);other.requestFocus();
        deadline=System.nanoTime()+2_000_000_000L;
        do{pump(loop);Thread.sleep(1);}while(!HostTextInput.active()&&System.nanoTime()<deadline);
        check(HostTextInput.active(),"focus handoff publishes the new input connection");
        HostTextInput.text("second",false,0,0);pump(loop);
        check(other.getText().toString().equals("second")&&editor.getText().toString().equals("replacement"),"retired editor receives no subsequent text");
        activity.setContentView(previous);previous.requestFocus();pump(loop);
        HostTextInput.text("ignored",false,0,0);pump(loop);
        check(!HostTextInput.active()&&other.getText().toString().equals("second"),"noneditor focus stops text input without changing retired content");
        System.out.println("PASS original EditText committed text, Unicode composition, selection and focus retirement");
    }
    private static void statistics()throws Exception{
        int atom=com.android.internal.util.FrameworkStatsLog.TOUCH_GESTURE_CLASSIFIED;
        android.util.StatsEvent event=android.util.StatsEvent.newBuilder().setAtomId(atom)
            .writeString(View.class.getName()).writeInt(com.android.internal.util.FrameworkStatsLog.TOUCH_GESTURE_CLASSIFIED__CLASSIFICATION__SINGLE_TAP)
            .usePooledBuffer().build();
        var buffer=android.util.StatsEvent.class.getDeclaredField("mBuffer");buffer.setAccessible(true);
        check(event.getAtomId()==atom&&event.getNumBytes()>0&&event.getNumBytes()<=event.getBytes().length&&buffer.get(event)!=null,"original StatsEvent builds a pooled gesture payload");
        android.util.StatsLog.write(event);
        check(buffer.get(event)==null,"original StatsLog releases the event buffer without an Android statistics collector");
        com.android.internal.util.FrameworkStatsLog.write(atom,View.class.getName(),com.android.internal.util.FrameworkStatsLog.TOUCH_GESTURE_CLASSIFIED__CLASSIFICATION__SINGLE_TAP);
    }
    public static final class OwnedActivity extends Activity {
        static boolean redirectOnCreate;
        final List<String> finishingCallbacks=new ArrayList<>();
        public View content;
        int configurationChanges;android.content.res.Configuration lastConfiguration;
        public void onConfigurationChanged(android.content.res.Configuration configuration){super.onConfigurationChanged(configuration);configurationChanges++;lastConfiguration=new android.content.res.Configuration(configuration);}
        boolean windowFocused;
        public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);windowFocused=focused;}
        int activityRequest=-1,activityResult;Intent activityData;Thread activityResultThread;
        protected void onActivityResult(int request,int result,Intent data){super.onActivityResult(request,result,data);activityRequest=request;activityResult=result;activityData=data;activityResultThread=Thread.currentThread();}
        int permissionRequest=-1;String[] permissions;int[] grants;Thread permissionThread;
        public void onRequestPermissionsResult(int request,String[] names,int[] results){permissionRequest=request;permissions=names;grants=results;permissionThread=Thread.currentThread();}
        protected void onCreate(Bundle saved){super.onCreate(saved);events.add("create");setTheme(android.R.style.Theme_Material_Light_NoActionBar);content=new View(this){protected void onDraw(Canvas canvas){canvas.drawColor(Color.RED);}};content.setId(123);content.setFocusable(true);content.setFocusableInTouchMode(true);content.setWillNotDraw(false);setContentView(content);if(redirectOnCreate){startActivity(new Intent(this,NextActivity.class));finish();}}
        protected void onStart(){super.onStart();events.add("start");}
        protected void onResume(){super.onResume();events.add("resume");}
        protected void onPause(){finishingCallbacks.add("pause:"+isFinishing());events.add("pause");super.onPause();}
        protected void onStop(){finishingCallbacks.add("stop:"+isFinishing());events.add("stop");super.onStop();}
        protected void onDestroy(){finishingCallbacks.add("destroy:"+isFinishing());events.add("destroy");super.onDestroy();}
    }
    public static final class NextActivity extends Activity {
        static NextActivity instance;static int creations,newIntents;static final List<String> lifecycle=new ArrayList<>();
        final List<String> finishingCallbacks=new ArrayList<>();
        static int windowTheme;static boolean finishOnCreate;
        protected void onCreate(Bundle state){if(windowTheme!=0)setTheme(windowTheme);super.onCreate(state);instance=this;creations++;lifecycle.add("create");if(finishOnCreate){finish();return;}setTitle("Next activity");View view=new View(this){protected void onDraw(Canvas canvas){canvas.drawColor(Color.BLUE);}};view.setWillNotDraw(false);setContentView(view);}
        protected void onStart(){super.onStart();lifecycle.add("start");}
        protected void onResume(){super.onResume();lifecycle.add("resume");}
        protected void onPause(){finishingCallbacks.add("pause:"+isFinishing());lifecycle.add("pause");super.onPause();}
        protected void onStop(){finishingCallbacks.add("stop:"+isFinishing());lifecycle.add("stop");super.onStop();}
        protected void onDestroy(){finishingCallbacks.add("destroy:"+isFinishing());lifecycle.add("destroy");super.onDestroy();}
        protected void onNewIntent(Intent intent){super.onNewIntent(intent);newIntents++;setIntent(intent);}
    }
    public static final class PermissionFragment extends Fragment {
        int permissionRequest=-1;int[] grants;
        public void onRequestPermissionsResult(int request,String[] names,int[] results){permissionRequest=request;grants=results;}
    }
    public static void main(String[] args)throws Exception{
        Path runtime=Path.of(args[0]).toRealPath(),data=Path.of(args[1]);Files.createDirectories(data);data=data.toRealPath();
        Path files=Files.createDirectories(data.resolve("files")),cache=Files.createDirectories(data.resolve("cache"));
        HostFilesystem.configure(runtime,data,files,cache,"org.example.window");
        System.setProperty("klepton.bridge",runtime.resolve("native/libklepton-jvm.dylib").toString());
        System.setProperty("klepton.graphics",runtime.resolve("native/libklepton-graphics.dylib").toString());
        System.setProperty("klepton.audio",runtime.resolve("native/libklepton-audio.dylib").toString());
        System.load(System.getProperty("klepton.bridge"));
        if(System.getenv("KL_ANDROID_ID_DIRECTORY")!=null)HostProcessIdentity.initializeIds();
        System.load(runtime.resolve("native/libnativebinding.dylib").toString());
        JvmNativeMemory.configure(runtime.resolve("native/libandroid_runtime.dylib"));JvmNativeMemory.initialize();
        System.load(System.getProperty("klepton.graphics"));
        String platformSocket=System.getProperty("android.fixture.platformSocket");
        if(platformSocket!=null)HostProcessIdentity.configure(10031,(int)new com.sun.security.auth.module.UnixSystem().getUid());
        for(int index=2;index<args.length;index++)System.load(Path.of(args[index]).toAbsolutePath().toString());
        statistics();
        int logTag=902001;android.util.EventLog.writeEvent(logTag,17);android.util.EventLog.writeEvent(logTag,19L);android.util.EventLog.writeEvent(logTag,.5f);android.util.EventLog.writeEvent(logTag,"window");android.util.EventLog.writeEvent(logTag,new Object[]{"entry",3});
        var log=new ArrayList<android.util.EventLog.Event>();android.util.EventLog.readEvents(new int[]{logTag},log);check(log.size()==5&&log.get(0).getData().equals(17)&&log.get(1).getData().equals(19L)&&log.get(2).getData().equals(.5f)&&log.get(3).getData().equals("window")&&Arrays.equals((Object[])log.get(4).getData(),new Object[]{"entry",3}),"original EventLog decoder and complete native writers");check(log.get(0).getProcessId()==android.os.Process.myPid()&&log.get(0).getUid()==android.os.Process.myUid(),"actual event process identity");
        Path apk=Path.of(System.getProperty("android.fixture.apk",data.resolve("owned.apk").toString()));SyntheticResources.apk(apk,"","Window",null);maclauncher.android.fixture.PackageManifest.add(apk,System.getProperty("android.fixture.manifest","window"));
        if(platformSocket!=null){
            System.out.println("PLATFORM_READY "+android.os.Process.myPid());System.out.flush();
            if(!"connect".equals(new java.io.BufferedReader(new java.io.InputStreamReader(System.in)).readLine()))throw new IllegalStateException("Platform enrollment must precede connection");
            HostRemoteServices.attachPlatform(platformSocket,new HostBinderRpc.Peer("android","android.system",Integer.getInteger("android.fixture.platformPid"),1000,"u:r:system_server:s0"));
        }
        FrameworkResources.configure(runtime.resolve("android-framework.jar"),List.of(apk));
        HostContext.hostPackage("org.example.window","1",1);HostContext.hostApplicationInfo(false,apk.toString(),data.toString(),data.toString(),28);
        HostContext.hostApplicationIdentity(android.os.Process.myUid(),0);HostContext.hostFilesDirectory(files.toString());HostContext.hostCacheDirectory(cache.toString());HostContext.hostClassLoader(HostWindowProbe.class.getClassLoader());
        Looper loop=HostLooper.main();
        HostPackages.inspectSources(List.of(apk));
        HostStorageService.install();
        HostDisplay.hostPublish(1,320,240,60);HostDisplayEvents.hostSetWakeup(()->{});HostApplication.prepare(HostWindowProbe.class.getClassLoader());Context context=HostApplication.applicationContext();Application app=new Application();HostApplication.attach(app,context);HostApplication.create(app);
        var activityManager=context.getSystemService(ActivityManager.class);
        check(activityManager.getAppTasks().isEmpty(),"original task enumeration is empty before any Activity exists");
        OwnedActivity activity=new OwnedActivity();HostActivity.attach(activity,context,app);SurfaceControl output=null;Throwable first=null;
        try{
            String androidId=android.provider.Settings.Secure.getString(context.getContentResolver(),android.provider.Settings.Secure.ANDROID_ID);
            check(androidId!=null&&androidId.matches("[0-9a-f]{16}"),"original Settings client receives a scoped Android identity");
            check(!Files.exists(data.resolve("system/android-id")),"the original shared Settings provider retains the identity without an app-private shadow store");
            try(var identity=context.getContentResolver().query(android.provider.Settings.Secure.getUriFor(android.provider.Settings.Secure.ANDROID_ID),new String[]{android.provider.Settings.NameValueTable.VALUE},null,null,null)){
                check(identity!=null&&identity.moveToFirst()&&identity.getString(0).equals(androidId),"Settings query and call share the same platform identity");
            }
            check(activity.getAssociatedDisplayId()==activity.getDisplayId()&&activity.isUiContext(),"activity display association");
            Context display=context.createDisplayContext(HostDisplay.host());check(display.getDisplayNoVerify().getDisplayId()==activity.getDisplayId()&&!display.isUiContext(),"display context platform identity");
            check(context.createConfigurationContext(new android.content.res.Configuration(context.getResources().getConfiguration())).isConfigurationContext(),"configuration context identity");
            check(context.createDeviceContext(0).getDeviceId()==0,"default device context");try{context.createDeviceContext(1);throw new AssertionError("foreign device context accepted");}catch(IllegalArgumentException expected){}
            HostActivity.start(activity,null);HostActivity.resume(activity);check(events.equals(List.of("create","start","resume")),"raw lifecycle ordering");
            var appTasks=activityManager.getAppTasks();check(appTasks.size()==1,"original ActivityManager enumerates the actual native task");
            var appTask=appTasks.getFirst();var taskInfo=appTask.getTaskInfo();int taskId=taskInfo.taskId;
            check(taskId==activity.getTaskId()&&taskInfo.numActivities==1&&taskInfo.baseActivity.equals(activity.getComponentName())&&taskInfo.topActivity.equals(activity.getComponentName()),"original AppTask metadata comes from the lifecycle owner");
            appTask.setExcludeFromRecents(true);check((appTask.getTaskInfo().baseIntent.getFlags()&Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS)!=0,"task exclusion is reflected in the original Intent metadata");
            appTask.setExcludeFromRecents(false);check((appTask.getTaskInfo().baseIntent.getFlags()&Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS)==0,"task exclusion can be cleared without changing the Activity Intent");
            Window window=activity.getWindow();check(window.getClass().getName().equals("com.android.internal.policy.PhoneWindow"),"original complete PhoneWindow");
            check(activity.findViewById(123)==activity.content,"original content lookup");window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);check((window.getAttributes().flags&WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)!=0,"original flags");
            activity.setTitle("Actual title");check(HostActivity.title(activity).equals("Actual title"),"native title observation");
            output=HostSurfaceControl.rootSurfaceControl("owned-window",320,240,new HostSurface.Target(){public boolean submit(int[] pixels,int w,int h,long id,long time){check(w==outputWidth&&h==outputHeight,"actual output dimensions");latestPixels=pixels.clone();frames++;return true;}public void resize(int w,int h){}});fixtureOutput=output;
            HostWindowManager.attachOutput(activity,output,320,240);HostActivity.owner(activity).focus(true);
            var serviceField=HostWindowManager.class.getDeclaredField("service");serviceField.setAccessible(true);Object windowService=serviceField.get(null);
            var embeddedField=HostWindowService.class.getDeclaredField("windows");embeddedField.setAccessible(true);WindowlessWindowManager embedded=(WindowlessWindowManager)embeddedField.get(windowService);
            var systemField=WindowlessWindowManager.class.getDeclaredField("mRealWm");systemField.setAccessible(true);IWindowSession systemSession=(IWindowSession)systemField.get(embedded);
            check(systemSession!=WindowManagerGlobal.getWindowSession()&&systemSession!=embedded&&systemSession.asBinder()!=WindowManagerGlobal.getWindowSession().asBinder(),"native server and embedded application sessions have distinct identities");
            window.getInsetsController().hide(WindowInsets.Type.ime());window.getInsetsController().show(WindowInsets.Type.ime());
            long deadline=System.nanoTime()+2_000_000_000L;while(frames==0&&System.nanoTime()<deadline){pump(loop);Thread.sleep(2);}
            check(activity.content.isAttachedToWindow()&&activity.content.getViewRootImpl()!=null,"original attached ViewRoot");check(frames>0,"original traversal produced pixels");
            String inputProbe=System.getProperty("android.fixture.input");if(inputProbe!=null){Class.forName(inputProbe).getMethod("run",Activity.class,Looper.class).invoke(null,activity,loop);return;}
            check(Arrays.stream(latestPixels).anyMatch(pixel->pixel==Color.RED),"original activity pixels reach the output");
            check(window.getDecorView().hasWindowFocus(),"native focus before the first traversal reaches the original view root");
            window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);pump(loop);
            check(window.getAttributes().token==activity.getActivityToken(),"ordinary Window attribute updates retain the Activity window token");
            check(WindowManagerGlobal.getInstance().getRootViews(activity.getActivityToken()).contains(activity.content.getViewRootImpl()),"original WindowManagerGlobal still resolves the Activity after an attribute update");
            HostActivity.owner(activity).focus(false);pump(loop);
            check(!window.getDecorView().hasWindowFocus()&&!activity.windowFocused,"native focus loss reaches the original Activity after attribute updates");
            HostActivity.owner(activity).focus(true);pump(loop);
            check(window.getDecorView().hasWindowFocus()&&activity.windowFocused,"native focus returns through the original ViewRoot and Activity after attribute updates");
            window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);pump(loop);
            check(context.getPackageManager().getPermissionControllerPackageName()==null,"no invented permission-controller package");
            activity.requestPermissions(new String[]{"android.permission.CAMERA","org.example.UNKNOWN_PERMISSION"},73);
            check(activity.permissionRequest==-1,"permission result is asynchronous");long permissionDeadline=System.nanoTime()+1_000_000_000L;while(activity.permissionRequest<0&&System.nanoTime()<permissionDeadline){pump(loop);Thread.sleep(1);}
            check(activity.permissionRequest==73&&Arrays.equals(activity.permissions,new String[]{"android.permission.CAMERA","org.example.UNKNOWN_PERMISSION"})&&Arrays.equals(activity.grants,new int[]{PackageManager.PERMISSION_DENIED,PackageManager.PERMISSION_DENIED}),"original Activity receives actual denied permission decisions: "+activity.permissionRequest+" "+Arrays.toString(activity.permissions)+" "+Arrays.toString(activity.grants));
            PermissionFragment permissionFragment=new PermissionFragment();activity.getFragmentManager().beginTransaction().add(permissionFragment,"permissions").commitNow();
            permissionFragment.requestPermissions(new String[]{"android.permission.ACCESS_FINE_LOCATION"},74);pump(loop);
            check(permissionFragment.permissionRequest==74&&Arrays.equals(permissionFragment.grants,new int[]{PackageManager.PERMISSION_DENIED}),"original Fragment result routing rejects undeclared location without a prompt");
            activity.getFragmentManager().beginTransaction().remove(permissionFragment).commitNow();
            java.util.concurrent.CompletableFuture.runAsync(()->activity.requestPermissions(new String[]{"org.example.UNKNOWN_PERMISSION"},75)).get(2,java.util.concurrent.TimeUnit.SECONDS);
            check(activity.permissionRequest==73,"worker permission submission does not run Activity callbacks on the caller");
            pump(loop);check(activity.permissionRequest==75&&activity.permissionThread==Thread.currentThread(),"original permission result returns through ActivityThread after a worker request");
            android.window.WindowOnBackInvokedDispatcher back=activity.content.getViewRootImpl().getOnBackInvokedDispatcher();
            android.window.OnBackInvokedCallback callback=()->{};
            java.util.concurrent.CompletableFuture.runAsync(()->{
                back.registerOnBackInvokedCallbackUnchecked(callback,android.window.OnBackInvokedDispatcher.PRIORITY_OVERLAY);
                if(back.getTopCallback()!=callback)throw new AssertionError("worker callback was not registered");
                back.unregisterOnBackInvokedCallback(callback);
                if(((HostWindowService)windowService).openSession(null).asBinder()==null)throw new AssertionError("worker session identity");
            }).get(2,java.util.concurrent.TimeUnit.SECONDS);
            check(back.getTopCallback()!=callback,"original back registration accepts worker callers without waiting for the UI thread");
            int[] keys={0};activity.content.requestFocus();activity.content.setOnKeyListener((v,key,event)->{if(key!=KeyEvent.KEYCODE_BUTTON_A)return false;keys[0]++;return true;});
            HostWindowManager.input(new KeyEvent(KeyEvent.ACTION_DOWN,KeyEvent.KEYCODE_BUTTON_A));pump(loop);check(keys[0]==1,"original ViewRoot input dispatch");
            textInput(activity,loop);
            View panel=new View(activity);WindowManager.LayoutParams params=new WindowManager.LayoutParams(40,20,WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,0,PixelFormat.TRANSLUCENT);params.token=window.getDecorView().getWindowToken();
            window.getWindowManager().addView(panel,params);pump(loop);check(panel.getViewRootImpl()!=null,"independent original panel ViewRoot");check(panel.hasWindowFocus()&&!window.getDecorView().hasWindowFocus(),"focus transfers to the new panel");window.getWindowManager().removeViewImmediate(panel);check(!panel.isAttachedToWindow(),"panel teardown");pump(loop);check(window.getDecorView().hasWindowFocus(),"focus returns after panel removal");
            AlertDialog dialog=new AlertDialog.Builder(activity).setMessage("Window callback").setPositiveButton("Close",(value,which)->value.dismiss()).create();dialog.show();pump(loop);
            View dialogView=dialog.getWindow().getDecorView();check(dialog.isShowing()&&HostWindowManager.topView(activity)==dialogView,"original dialog owns the top application window");check(dialogView.hasWindowFocus()&&!window.getDecorView().hasWindowFocus(),"dialog receives native focus");
            check(dialogView.getHeight()>40&&dialogView.getHeight()<=240,"wrap-content dialog measures within the native display: "+dialogView.getWidth()+"x"+dialogView.getHeight()+", measured="+dialogView.getMeasuredHeight()+", parameters="+dialog.getWindow().getAttributes());
            check(Arrays.stream(latestPixels).anyMatch(pixel->Color.alpha(pixel)>0&&Color.red(pixel)>200&&Color.green(pixel)>200&&Color.blue(pixel)>200),"original dialog text/background pixels reach the output");
            Dialog secondDialog=new Dialog(activity);secondDialog.setContentView(new View(activity));secondDialog.show();pump(loop);check(secondDialog.getWindow().getDecorView().isAttachedToWindow()&&dialogView.isAttachedToWindow(),"multiple root windows share the activity token without treating it as an IWindow");secondDialog.dismiss();pump(loop);
            View closeButton=dialog.getButton(DialogInterface.BUTTON_POSITIVE);int[] buttonLocation=new int[2];closeButton.getLocationOnScreen(buttonLocation);float buttonX=buttonLocation[0]+closeButton.getWidth()/2f,buttonY=buttonLocation[1]+closeButton.getHeight()/2f;
            HostWindowManager.input(HostInput.motion(MotionEvent.ACTION_DOWN,buttonX,buttonY));pump(loop);HostWindowManager.input(HostInput.motion(MotionEvent.ACTION_UP,buttonX,buttonY));
            long closeDeadline=System.nanoTime()+1_000_000_000L;while((dialog.isShowing()||dialogView.isAttachedToWindow())&&System.nanoTime()<closeDeadline){pump(loop);Thread.sleep(2);}
            check(!dialog.isShowing()&&!dialogView.isAttachedToWindow(),"native display coordinates reach the original dialog button and retire its view root: showing="+dialog.isShowing()+", attached="+dialogView.isAttachedToWindow());
            List<String> beforeNavigation=List.copyOf(events);IBinder response=new Binder();Bundle extras=new Bundle();extras.putBinder("response",response);
            activity.startActivityForResult(new Intent(activity,NextActivity.class).putExtras(extras),91);
            long navigationDeadline=System.nanoTime()+2_000_000_000L;while((NextActivity.instance==null||!NextActivity.instance.getWindow().getDecorView().hasWindowFocus())&&System.nanoTime()<navigationDeadline){pump(loop);Thread.sleep(2);}
            NextActivity next=NextActivity.instance;check(next!=null&&next.getIntent().getExtras().getBinder("response")==response,"original Activity launch preserves Binder extras");
            taskInfo=appTask.getTaskInfo();check(taskInfo.taskId==taskId&&taskInfo.numActivities==2&&taskInfo.topActivity.equals(next.getComponentName())&&taskInfo.baseActivity.equals(activity.getComponentName()),"AppTask follows navigation without another task registry");
            check(next.getCallingActivity().equals(activity.getComponentName())&&next.getCallingPackage().equals(activity.getPackageName()),"original calling Activity and package metadata");
            var resultOwner=HostActivity.owner(next);var resultField=HostActivity.class.getDeclaredField("resultTo");resultField.setAccessible(true);
            IBinder resultToken=(IBinder)resultField.get(resultOwner);var pair=ParcelFileDescriptor.createSocketPair();
            var peer=new HostBinderRpc.Peer(activity.getPackageName(),activity.getPackageName(),android.os.Process.myPid(),android.os.Process.myUid());
            var tokenServer=HostBinderRpc.serve(pair[0],peer,resultToken,2);var tokenClient=HostBinderRpc.connect(pair[1],peer,2);pair[0].close();pair[1].close();
            try{resultField.set(resultOwner,tokenClient.root());check(next.getCallingActivity().equals(activity.getComponentName())&&next.getCallingPackage().equals(activity.getPackageName()),"remote original result token resolves the actual calling Activity");}
            finally{resultField.set(resultOwner,resultToken);tokenClient.close();tokenServer.close();tokenClient.closed().toCompletableFuture().get(5,java.util.concurrent.TimeUnit.SECONDS);tokenServer.closed().toCompletableFuture().get(5,java.util.concurrent.TimeUnit.SECONDS);}
            check(NextActivity.lifecycle.equals(List.of("create","start","resume")),"original secondary Activity lifecycle: "+NextActivity.lifecycle);
            check(!activity.getWindow().getDecorView().hasWindowFocus()&&next.getWindow().getDecorView().hasWindowFocus(),"native focus follows the foreground Activity");
            check(HostActivity.owner(activity).title().equals("Next activity")&&Arrays.stream(latestPixels).anyMatch(pixel->pixel==Color.BLUE),"shared native output and title follow the next Activity");
            appTask.startActivity(next,new Intent(next,NextActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP).putExtra("value",27),null);pump(loop);
            check(NextActivity.creations==1&&NextActivity.newIntents==1&&next.getIntent().getIntExtra("value",0)==27,"singleTop reuses original onNewIntent handling");
            next.setResult(17,new Intent().putExtras(extras));next.finish();navigationDeadline=System.nanoTime()+2_000_000_000L;while((activity.activityRequest!=91||!activity.getWindow().getDecorView().hasWindowFocus())&&System.nanoTime()<navigationDeadline){pump(loop);Thread.sleep(2);}
            check(next.isDestroyed()&&activity.activityRequest==91&&activity.activityResult==17&&activity.activityData.getExtras().getBinder("response")==response,"original Activity result and destruction preserve Binder response");
            check(activity.getWindow().getDecorView().hasWindowFocus()&&Arrays.stream(latestPixels).anyMatch(pixel->pixel==Color.RED),"return restores the prior Activity and native pixels");
            check(events.subList(beforeNavigation.size(),events.size()).equals(List.of("pause","stop","start","resume","pause","resume")),"original navigation and result lifecycle: "+events);
            var windowSession=WindowManagerGlobal.getWindowSession();var windowManager=WindowManagerGlobal.getWindowManagerService();
            HostWindowManager.detachPresentation();check(!window.getDecorView().isAttachedToWindow(),"output detach retires view roots");
            HostWindowManager.attachOutput(activity,output,320,240);HostWindowManager.focusOutput(true);pump(loop);
            check(WindowManagerGlobal.getWindowSession()==windowSession&&WindowManagerGlobal.getWindowManagerService()==windowManager,"process window service/session survive output replacement");
            check(window.getDecorView().isAttachedToWindow()&&window.getDecorView().hasWindowFocus(),"new output uses the same original window service");
            var fragment=android.content.res.LayoutInflationProbe.fragments(activity);
            appTask.finishAndRemoveTask();pump(loop);android.content.res.LayoutInflationProbe.destroyed(fragment);var expected=new ArrayList<>(beforeNavigation);expected.addAll(List.of("pause","stop","start","resume","pause","resume","pause","stop","destroy"));check(events.equals(expected),"original permission-result lifecycle cycles and reverse retirement: "+events);check(!activity.content.isAttachedToWindow(),"original ViewRoot retirement");
            check(activityManager.getAppTasks().isEmpty(),"task enumeration retires with the actual Activity stack");
            try{appTask.getTaskInfo();throw new AssertionError("retired task capability remained live");}catch(IllegalArgumentException expectedRetired){}
            var visible=new OwnedActivity();HostActivity.attach(visible,context,app);HostActivity.start(visible,null);HostWindowManager.detachOutput();HostWindowManager.attachOutput(visible,output,320,240);HostActivity.resume(visible);pump(loop);
            for(int theme:new int[]{android.R.style.Theme_Translucent_NoTitleBar,android.R.style.Theme_Material_Light_Dialog}){
                NextActivity.windowTheme=theme;int begin=events.size();visible.startActivity(new Intent(visible,NextActivity.class));pump(loop);
                check(events.subList(begin,events.size()).equals(List.of("pause")),"translucent/floating activity pauses without stopping its visible predecessor");
                var overlay=NextActivity.instance;check(!overlay.isDestroyed()&&visible.content.isAttachedToWindow(),"overlay retains the previous original ViewRoot");
                overlay.finish();pump(loop);check(events.subList(begin,events.size()).equals(List.of("pause","resume")),"return from overlay preserves the existing started surface");
            }
            NextActivity.windowTheme=android.R.style.Theme_Translucent_NoTitleBar;visible.startActivity(new Intent(visible,NextActivity.class));pump(loop);var translucent=NextActivity.instance;
            NextActivity.windowTheme=android.R.style.Theme_Material_Light_NoActionBar;int beforeCover=events.size();translucent.startActivity(new Intent(translucent,NextActivity.class));pump(loop);var opaque=NextActivity.instance;
            check(events.subList(beforeCover,events.size()).equals(List.of("stop")),"opaque activity stops every covered predecessor, including beneath a translucent activity");
            opaque.finish();pump(loop);check(events.subList(beforeCover,events.size()).equals(List.of("stop","start")),"uncovering a translucent activity restarts its visible underlying activity without resuming it");
            translucent.finish();pump(loop);
            NextActivity.finishOnCreate=true;NextActivity.windowTheme=0;int beforeFinished=events.size();visible.startActivity(new Intent(visible,NextActivity.class));pump(loop);NextActivity.finishOnCreate=false;
            check(!events.subList(beforeFinished,events.size()).contains("stop"),"an immediately finishing result activity never destroys the predecessor surface");
            HostActivity.owner(visible).finish();pump(loop);
            OwnedActivity.redirectOnCreate=true;var trampoline=new OwnedActivity();HostActivity.attach(trampoline,context,app);HostActivity.start(trampoline,null);
            check(trampoline.isFinishing(),"startup trampoline requests its own finish in onCreate");
            HostWindowManager.detachOutput();HostWindowManager.attachOutput(trampoline,output,320,240);
            var taskOwner=HostActivity.owner(trampoline);taskOwner.drain();
            var successor=NextActivity.instance;check(successor!=next&&!successor.isDestroyed()&&!taskOwner.closing(),"queued startup successor remains in the same live presentation task");
            var successorTask=activityManager.getAppTasks().getFirst();check(successorTask.getTaskInfo().numActivities==1&&successorTask.getTaskInfo().taskId!=taskId&&successorTask.getTaskInfo().topActivity.equals(successor.getComponentName()),"startup redirect retires the trampoline while retaining its successor");
            successorTask.finishAndRemoveTask();pump(loop);check(taskOwner.closing(),"redirected task closes only after its last Activity retires");
        }catch(Throwable error){first=error;throw error;}finally{
            try{try{if(!activity.isDestroyed())HostActivity.finish(activity);}finally{HostWindowManager.shutdown();if(output!=null)output.release();HostApplication.shutdown();if(platformSocket!=null)HostRemoteServices.shutdown();HostDisplayEvents.hostShutdown();HostDisplay.hostShutdown();HostDisplay.hostRemove();HostContext.hostShutdown();FrameworkResources.shutdown();HostLooper.close(loop);retirePreferences();}}
            catch(Throwable cleanup){if(first!=null)first.addSuppressed(cleanup);else throw cleanup;}
        }
        System.out.println("PASS "+checks+" upstream Activity, PhoneWindow, ViewRoot pixels, input and window lifetime checks");
    }
}
