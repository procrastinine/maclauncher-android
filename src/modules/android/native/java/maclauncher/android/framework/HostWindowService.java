package maclauncher.android.framework;

import android.content.Context;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.*;
import android.view.*;
import java.util.*;

/** Window-server boundary for Android trees embedded in the existing SDL window. */
public final class HostWindowService extends IWindowManager.Default implements AutoCloseable {
    private final Context context;
    private final boolean originalHardwareRenderer;
    boolean usesOriginalHardwareRenderer(){return originalHardwareRenderer;}
    private final Thread owner=Thread.currentThread();
    private final SystemSession systemSession=new SystemSession();
    private final Session session=new Session();
    private volatile WindowlessWindowManager windows;
    private SurfaceControl container;
    private final Rect outputFrame=new Rect();
    private final InsetsState outputInsets=new InsetsState();
    private volatile int width,height;
    private boolean output;
    private final Set<IBinder> tokens=Collections.synchronizedSet(Collections.newSetFromMap(new IdentityHashMap<>()));
    private final Set<IWindowSessionCallback> callbacks=Collections.synchronizedSet(Collections.newSetFromMap(new IdentityHashMap<>()));
    private final IBinder endpoint=new Binder("android.view.IWindowManager");
    private volatile boolean closed,touchMode;
    private float animatorScale=1f;
    public HostWindowService(Context context){this(context,false);}
    /** The VM bootstrap identifies the native Surface/HWUI implementation it owns. */
    public HostWindowService(Context context,boolean originalHardwareRenderer){
        this.context=Objects.requireNonNull(context);this.originalHardwareRenderer=originalHardwareRenderer;
        touchMode=context.getResources().getBoolean(com.android.internal.R.bool.config_defaultInTouchMode);
        ((Binder)endpoint).attachInterface(this,"android.view.IWindowManager");
    }
    public void install(){check();try{HostServices.manager().addService(Context.WINDOW_SERVICE,endpoint,false,0);}catch(RemoteException error){throw error.rethrowAsRuntimeException();}if(!originalHardwareRenderer)ThreadedRenderer.sRendererEnabled=false;}
    @Override public IBinder asBinder(){return endpoint;}
    private void checkOpen(){if(closed)throw new IllegalStateException("Window service has closed");}
    private void check(){if(Thread.currentThread()!=owner)throw new IllegalStateException("Native window lifecycle requires the owner thread");checkOpen();}
    private void display(int id){if(id!=Display.DEFAULT_DISPLAY)throw new IllegalArgumentException("Display is not owned by this process");}
    @Override public IWindowSession openSession(IWindowSessionCallback callback){checkOpen();if(callback!=null)callbacks.add(callback);return session.endpoint;}
    @Override public void getInitialDisplaySize(int id,Point out){getBaseDisplaySize(id,out);}
    @Override public void getBaseDisplaySize(int id,Point out){display(id);var metrics=context.getResources().getDisplayMetrics();out.set(metrics.widthPixels,metrics.heightPixels);}
    @Override public int getInitialDisplayDensity(int id){return getBaseDisplayDensity(id);}
    @Override public int getBaseDisplayDensity(int id){display(id);return context.getResources().getDisplayMetrics().densityDpi;}
    @Override public boolean hasNavigationBar(int id){display(id);return false;}
    @Override public void getStableInsets(int id,Rect out){display(id);out.setEmpty();}
    @Override public boolean isInTouchMode(int id){display(id);return touchMode;}
    @Override public void setInTouchMode(boolean value,int id){checkOpen();display(id);touchMode=value;}
    @Override public void setInTouchModeOnAllDisplays(boolean value){setInTouchMode(value,Display.DEFAULT_DISPLAY);}
    @Override public float getCurrentAnimatorScale(){return animatorScale;}
    @Override public float getAnimationScale(int which){return 1f;}
    @Override public float[] getAnimationScales(){return new float[]{1f,1f,animatorScale};}
    @Override public boolean isWindowToken(IBinder token){return tokens.contains(token);}
    public void attachToken(IBinder token){check();tokens.add(Objects.requireNonNull(token));}
    public void detachToken(IBinder token){check();tokens.remove(token);}
    @Override public void addWindowToken(IBinder token,int type,int display,Bundle options){throw new SecurityException("System window-token creation is unavailable");}
    @Override public void removeWindowToken(IBinder token,int display){throw new SecurityException("System window-token removal is unavailable");}
    @Override public void setForcedDisplaySize(int display,int width,int height){throw new SecurityException("Display configuration belongs to the native window");}
    @Override public void setForcedDisplayDensityForUser(int display,int density,int user){throw new SecurityException("Display density belongs to the native window");}
    @Override public void lockNow(Bundle options){throw new SecurityException("Device locking is unavailable");}
    @Override public boolean startViewServer(int port){return false;}
    @Override public boolean stopViewServer(){return false;}
    @Override public boolean isViewServerRunning(){return false;}
    private synchronized WindowlessWindowManager windows(){checkOpen();if(windows==null){
        container=new SurfaceControl.Builder().setName("Android application windows").setContainerLayer().setHidden(false).build();
        windows=new WindowlessWindowManager(context.getResources().getConfiguration(),container,null){
            @Override protected SurfaceControl getParentSurface(IWindow window,WindowManager.LayoutParams attrs){
                // Application tokens identify an Activity, not an IWindow. The
                // embedded manager's default assumes every later window is a
                // subwindow; independent Dialog/Activity roots instead belong
                // beside each other under the native output container.
                if(attrs.type<WindowManager.LayoutParams.FIRST_SUB_WINDOW||attrs.type>WindowManager.LayoutParams.LAST_SUB_WINDOW)return container;
                return super.getParentSurface(window,attrs);
            }
        };
        // The original constructor obtains the application session from WindowManagerGlobal.
        // Its server forwarding must instead reach the native window owner. These are
        // distinct endpoints: forwarding into the embedded compositor would form a cycle.
        HostUiAccess.set(windows,WindowlessWindowManager.class,"mRealWm",systemSession);
        updateOutputBounds(width,height);
        windows.setInsetsState(outputInsets);
    }return windows;}
    public void attachOutput(SurfaceControl surface,int width,int height){check();if(output)throw new IllegalStateException("Native window output is already attached");windows();this.width=width;this.height=height;try(var transaction=new SurfaceControl.Transaction()){transaction.reparent(container,Objects.requireNonNull(surface)).show(container).apply();}output=true;resizeOutput(width,height);}
    private void updateOutputBounds(int width,int height){var metrics=context.getResources().getDisplayMetrics();outputFrame.set(0,0,width>0?width:metrics.widthPixels,height>0?height:metrics.heightPixels);outputInsets.setDisplayFrame(outputFrame);}
    public void resizeOutput(int width,int height){check();this.width=width;this.height=height;updateOutputBounds(width,height);if(windows!=null)windows.setConfiguration(new android.content.res.Configuration(context.getResources().getConfiguration()));}
    public void detachOutput(){check();if(container!=null&&output)try(var transaction=new SurfaceControl.Transaction()){transaction.reparent(container,null).apply();}output=false;}
    @Override public void close(){if(closed)return;check();detachOutput();if(container!=null){try(var transaction=new SurfaceControl.Transaction()){transaction.remove(container).apply();}container.release();container=null;}windows=null;closed=true;callbacks.clear();tokens.clear();try{HostServices.manager().tryUnregisterService(Context.WINDOW_SERVICE,endpoint);}catch(RemoteException error){throw error.rethrowAsRuntimeException();}}
    /** The macOS server endpoint has no Android input channels or software IME.
     * WindowlessWindowManager owns embedded surfaces; SDL owns physical input.
     * The upstream empty server operations therefore terminate here, never at
     * the application compositor. */
    private final class SystemSession extends IWindowSession.Default {
        private final Binder binder=new Binder("android.view.IWindowSession");
        SystemSession(){binder.attachInterface(this,"android.view.IWindowSession");}
        @Override public IBinder asBinder(){return binder;}
        @Override public void remove(IBinder token){checkOpen();}
        @Override public void updateRequestedVisibleTypes(IWindow window,int types,android.view.inputmethod.ImeTracker.Token tracker){checkOpen();}
        @Override public void grantInputChannel(int display,SurfaceControl surface,IBinder client,android.window.InputTransferToken host,int flags,int privateFlags,int features,int type,IBinder window,android.window.InputTransferToken transfer,String name,InputChannel out)throws RemoteException{throw new RemoteException("Native window input does not use Android input channels");}
        @Override public void updateInputChannel(IBinder channel,int display,SurfaceControl surface,int flags,int privateFlags,int features,android.graphics.Region region)throws RemoteException{throw new RemoteException("Native window input does not use Android input channels");}
    }
    /** Full AOSP windowless session behind one local Binder endpoint. */
    private final class Session implements java.lang.reflect.InvocationHandler {
        final Binder binder=new Binder("android.view.IWindowSession");
        final IWindowSession endpoint=(IWindowSession)java.lang.reflect.Proxy.newProxyInstance(IWindowSession.class.getClassLoader(),new Class<?>[]{IWindowSession.class},this);
        Session(){binder.attachInterface(endpoint,"android.view.IWindowSession");}
        @Override public Object invoke(Object proxy,java.lang.reflect.Method method,Object[] supplied)throws Throwable{
            Object[] args=supplied==null?new Object[0]:supplied;String name=method.getName();
            if(method.getDeclaringClass()==Object.class)return switch(name){case "toString"->"Local Android window session";case "hashCode"->System.identityHashCode(proxy);case "equals"->proxy==args[0];default->throw new AssertionError(name);};
            if(name.equals("asBinder"))return binder;
            // A local Binder endpoint accepts the same callers as the system
            // service. The upstream windowless server synchronizes its state;
            // dispatching to the UI queue here would deadlock renderer waits.
            checkOpen();
            if(name.equals("grantInputChannel")||name.equals("updateInputChannel"))throw new IllegalStateException("The native window owns input channels");
            if(args.length>1&&args[1] instanceof WindowManager.LayoutParams original){
                var attrs=new WindowManager.LayoutParams();attrs.copyFrom(original);attrs.inputFeatures|=WindowManager.LayoutParams.INPUT_FEATURE_NO_INPUT_CHANNEL;
                args=args.clone();args[1]=attrs;
            }
            try{
                WindowlessWindowManager embedded=windows();Object result=method.invoke(embedded,args);
                if(name.startsWith("addToDisplay")&&args.length>1&&args[0] instanceof IWindow window&&args[1] instanceof WindowManager.LayoutParams attrs){
                    if(attrs.type<WindowManager.LayoutParams.FIRST_SUB_WINDOW||attrs.type>WindowManager.LayoutParams.LAST_SUB_WINDOW){
                        // An application window is attached to the native display, just as a
                        // subwindow is attached to its parent. Keep the original layout
                        // parameters: WindowlessWindowLayout owns wrap-content and gravity.
                        synchronized(embedded){
                            var states=(Map<?,?>)HostUiAccess.get(embedded,WindowlessWindowManager.class,"mStateForWindow");Object state=states.get(window.asBinder());
                            if(state!=null)HostUiAccess.set(state,state.getClass(),"mAttachedFrame",outputFrame);
                        }
                        for(Object argument:args)if(argument instanceof Rect attached)attached.set(outputFrame);
                    }
                    for(Object argument:args)if(argument instanceof InsetsState insets)insets.set(outputInsets);
                }
                return result;
            }catch(java.lang.reflect.InvocationTargetException error){throw error.getCause();}
        }
    }
}
