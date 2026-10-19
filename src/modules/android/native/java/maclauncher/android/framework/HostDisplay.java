package maclauncher.android.framework;

import android.content.Context;
import android.graphics.Point;
import android.hardware.display.*;
import android.os.*;
import android.util.DisplayMetrics;
import android.view.*;
import java.util.*;

/** Native committed-window observations behind the unchanged Android display clients. */
public final class HostDisplay extends IDisplayManager.Default {
    private static final HostDisplay SERVICE=new HostDisplay();
    private static final Object LOCK=new Object();
    private static Thread owner;
    private static boolean installed,stopped;
    private static int width=512,height=384,nextMode=1,activeCallbacks;
    private static int densityDpi=160;
    private static long screen;
    private static DisplayInfo info;
    private final Map<IDisplayManagerCallback,Long> callbacks=new IdentityHashMap<>();
    private HostDisplay(){}
    public static void install(){synchronized(LOCK){if(installed)return;HostServices.register(Context.DISPLAY_SERVICE,SERVICE,"android.hardware.display.IDisplayManager");installed=true;}}
    private static void owner(boolean removing){if(owner==null)owner=Thread.currentThread();else if(owner!=Thread.currentThread())throw new IllegalStateException("Display observations require the native window owner");if(stopped&&!removing)throw new IllegalStateException("Display callbacks have stopped");}
    public static Display host(){install();Display value=DisplayManagerGlobal.getInstance().getCompatibleDisplay(Display.DEFAULT_DISPLAY,new DisplayAdjustments());if(value==null)throw new IllegalStateException("Android display is not bound to a native window");return value;}
    public static long hostScreenId(){synchronized(LOCK){if(info==null)throw new IllegalStateException("Android display is not bound to a native window");return screen;}}
    public static void metrics(DisplayMetrics out){Objects.requireNonNull(out);synchronized(LOCK){out.setToDefaults();out.widthPixels=out.noncompatWidthPixels=width;out.heightPixels=out.noncompatHeightPixels=height;out.density=out.noncompatDensity=out.scaledDensity=densityDpi/160f;out.densityDpi=out.noncompatDensityDpi=densityDpi;out.xdpi=out.ydpi=out.noncompatXdpi=out.noncompatYdpi=densityDpi;}}
    public static void hostViewport(int w,int h,float ignoredLegacyRate){if(w<1||h<1)throw new IllegalArgumentException("Invalid viewport");synchronized(LOCK){owner(false);if(info==null){width=w;height=h;}}}
    /** Called only after native surface storage and its real screen observation commit. */
    public static boolean hostPublish(long screenId,int w,int h,float refresh){
        return hostPublish(screenId,w,h,refresh,1);
    }
    public static boolean hostPublish(long screenId,int w,int h,float refresh,float density){
        return publish(screenId,w,h,refresh,density,Display.TYPE_VIRTUAL);
    }
    /** Observe the actual main screen without allocating a window or display clock. */
    public static native void initializeHeadless();
    private static void hostPublishScreen(long screenId,int w,int h,float refresh,float density,boolean internal){publish(screenId,w,h,refresh,density,internal?Display.TYPE_INTERNAL:Display.TYPE_EXTERNAL);}
    private static boolean publish(long screenId,int w,int h,float refresh,float density,int type){
        boolean window=type==Display.TYPE_VIRTUAL;
        if(screenId<=0||screenId>0xffffffffL||w<1||h<1||!Float.isFinite(refresh)||refresh<=0||!Float.isFinite(density)||density<=0||density>Integer.MAX_VALUE/160f)throw new IllegalArgumentException("Invalid native display observation");
        int nextDensity=Math.max(1,Math.round(density*160));
        int event;boolean rateChanged;
        synchronized(LOCK){owner(false);DisplayInfo before=info;
            boolean modeChanged=before==null||before.logicalWidth!=w||before.logicalHeight!=h||Float.compare(before.getRefreshRate(),refresh)!=0;
            if(before!=null&&screen==screenId&&!modeChanged&&densityDpi==nextDensity&&before.type==type)return false;
            if(modeChanged&&nextMode==Integer.MAX_VALUE)throw new IllegalStateException("Display mode identity capacity exceeded");
            Display.Mode mode=modeChanged?new Display.Mode(nextMode++,w,h,refresh):before.getMode();
            var value=new DisplayInfo();value.displayId=Display.DEFAULT_DISPLAY;value.type=type;
            value.name=window?"Android window":"Mac display";value.uniqueId=(window?"window:":"screen:")+Long.toUnsignedString(screenId);value.ownerUid=android.os.Process.myUid();
            value.ownerPackageName=HostContext.hostApplicationInfoSnapshot().packageName;
            value.appWidth=value.logicalWidth=value.smallestNominalAppWidth=value.largestNominalAppWidth=w;
            value.appHeight=value.logicalHeight=value.smallestNominalAppHeight=value.largestNominalAppHeight=h;
            value.logicalDensityDpi=nextDensity;value.physicalXDpi=value.physicalYDpi=nextDensity;
            value.modeId=value.defaultModeId=mode.getModeId();value.supportedModes=value.appsSupportedModes=new Display.Mode[]{mode};
            value.renderFrameRate=refresh;value.supportedRefreshRates=new float[]{refresh};value.state=value.committedState=Display.STATE_ON;
            // App and compositor share the native display-clock phase. Android encodes
            // the compositor phase as one period minus the deadline plus a 1 ms allowance.
            value.appVsyncOffsetNanos=0;
            value.presentationDeadlineNanos=(long)(1_000_000_000d/refresh)+1_000_000L;
            value.supportedColorModes=new int[]{Display.COLOR_MODE_DEFAULT};value.userDisabledHdrTypes=new int[0];
            value.hdrCapabilities=new Display.HdrCapabilities();value.brightnessMinimum=value.brightnessMaximum=value.brightnessDefault=value.brightnessDim=Float.NaN;
            value.hdrSdrRatio=1;value.canHostTasks=window;
            rateChanged=before!=null&&Float.compare(before.getRefreshRate(),refresh)!=0;
            event=before==null?1:2;screen=screenId;width=w;height=h;densityDpi=nextDensity;info=value;
        }
        install();DisplayManagerGlobal.getInstance().disableLocalDisplayInfoCaches();
        HostInput.viewportChanged(w,h);FrameworkResources.viewportChanged();SERVICE.event(event);if(rateChanged)SERVICE.event(8);return true;
    }
    @Override public DisplayInfo getDisplayInfo(int id){synchronized(LOCK){return id==Display.DEFAULT_DISPLAY&&info!=null?new DisplayInfo(info):null;}}
    @Override public int[] getDisplayIds(boolean disabled){synchronized(LOCK){return info==null?new int[0]:new int[]{Display.DEFAULT_DISPLAY};}}
    @Override public boolean isUidPresentOnDisplay(int uid,int id){return uid==android.os.Process.myUid()&&getDisplayInfo(id)!=null;}
    @Override public Point getStableDisplaySize(){synchronized(LOCK){return info==null?new Point():new Point(width,height);}}
    @Override public void registerCallback(IDisplayManagerCallback callback){registerCallbackWithEventMask(callback,~0L);}
    @Override public void registerCallbackWithEventMask(IDisplayManagerCallback callback,long mask){synchronized(LOCK){Objects.requireNonNull(callback);if(stopped)return;callbacks.put(callback,mask);}}
    private void event(int event){
        List<Map.Entry<IDisplayManagerCallback,Long>> snapshot;
        synchronized(LOCK){if(stopped)return;snapshot=new ArrayList<>(callbacks.entrySet());}
        long flag=event==8?64L:1L<<(event-1);
        for(var callback:snapshot)if((callback.getValue()&flag)!=0)try{callback.getKey().onDisplayEvent(Display.DEFAULT_DISPLAY,event);}catch(RemoteException gone){synchronized(LOCK){callbacks.remove(callback.getKey());}}
    }
    /** Raw delegate callbacks are wrapped only at the native source-lifetime boundary. */
    public static boolean enterCallback(){synchronized(LOCK){if(stopped)return false;activeCallbacks++;return true;}}
    public static void leaveCallback(){synchronized(LOCK){if(activeCallbacks<=0)throw new IllegalStateException("Unbalanced display callback");activeCallbacks--;}}
    public static boolean hostHasInFlightCallbacks(){synchronized(LOCK){return activeCallbacks!=0;}}
    public static void hostShutdown(){
        synchronized(LOCK){owner(true);stopped=true;SERVICE.callbacks.clear();}
        if(installed){var global=DisplayManagerGlobal.getInstance();for(var delegate:global.getDisplayListeners())global.unregisterDisplayListener(delegate.mListener);}
    }
    /** The process still has its physical display after a native output closes. */
    public static void hostDetachOutput(){synchronized(LOCK){owner(true);if(stopped)return;}initializeHeadless();}
    /** Keep the last real display snapshot through guest renderer destruction. */
    public static void hostRemove(){synchronized(LOCK){owner(true);if(activeCallbacks!=0)throw new IllegalStateException("Display callbacks are still active");info=null;screen=0;}if(installed)DisplayManagerGlobal.getInstance().disableLocalDisplayInfoCaches();}
    @Override public int createVirtualDisplay(VirtualDisplayConfig config,IVirtualDisplayCallback callback,android.media.projection.IMediaProjection projection,String packageName){return Display.INVALID_DISPLAY;}
    @Override public float getBrightness(int display){return Float.NaN;}
    @Override public void setBrightness(int display,float brightness){throw new SecurityException("Desktop brightness is not an Android application control");}
    @Override public void setTemporaryBrightness(int display,float brightness){setBrightness(display,brightness);}
    @Override public int[] getUserDisabledHdrTypes(){return new int[0];}
    @Override public int[] getSupportedHdrOutputTypes(){return new int[0];}
    @Override public Display.Mode getSystemPreferredDisplayMode(int id){var value=getDisplayInfo(id);return value==null?null:value.getMode();}
    @Override public float getHighestHdrSdrRatio(int id){return 1f;}
    @Override public boolean requestDisplayPower(int id,int state){return false;}
}
