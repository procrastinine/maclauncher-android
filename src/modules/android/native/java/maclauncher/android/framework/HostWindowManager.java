package maclauncher.android.framework;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.view.*;
import java.lang.reflect.Field;
import java.util.*;

/** Native output and SDL event owner beneath Android's original window manager and view roots. */
public final class HostWindowManager {
    private static HostWindowService service;
    private static Activity owner;
    @FunctionalInterface public interface OrientationOutput { void request(Activity activity,boolean landscape); }
    private static OrientationOutput orientationOutput;
    private static boolean outputFocused;
    private static ViewRootImpl focusedRoot;
    private static ViewRootImpl touchRoot;
    private static final Field windowFrame=HostUiAccess.field(ViewRootImpl.class,"mWinFrameInScreen");
    public interface InputOutput {
        boolean input(InputEvent event,java.util.function.Consumer<Boolean> completion);
        void wheel(float x,float y,float horizontal,float vertical,boolean allowTouch);
        void text(String text,boolean composing,int start,int length);
        void shortcut(int code);
    }
    private static InputOutput inputOutput;
    /** A scoped child output borrows the existing physical input owner. */
    public static void inputOutput(InputOutput output){check();if(inputOutput!=null&&output!=null)throw new IllegalStateException("Input output is already delegated");HostScrollInput.cancel();HostInput.cancelTouches(true);InputOutput previous=inputOutput;for(var entry:List.copyOf(inputCompletions.entrySet()))if(entry.getValue().output==previous&&previous!=null)completeDelegated(entry.getKey(),entry.getValue(),false);inputOutput=output;updateFocus(null);}
    static boolean delegatedWheel(float x,float y,float horizontal,float vertical,boolean allowTouch){check();if(HostActivity.hostSleeping())return true;if(inputOutput==null)return false;inputOutput.wheel(x,y,horizontal,vertical,allowTouch);return true;}
    static boolean delegatedText(String text,boolean composing,int start,int length){check();if(HostActivity.hostSleeping())return true;if(inputOutput==null)return false;inputOutput.text(text,composing,start,length);return true;}
    static boolean delegatedShortcut(int code){check();if(HostActivity.hostSleeping())return true;if(inputOutput==null)return false;inputOutput.shortcut(code);return true;}
    private record InputCompletion(java.util.function.Consumer<Boolean> callback,InputOutput output,int sequence){}
    private static final Map<InputEvent,InputCompletion> inputCompletions=new IdentityHashMap<>();
    private static void completeDelegated(InputEvent event,InputCompletion ticket,boolean handled){check();if(inputCompletions.get(event)!=ticket)return;inputCompletions.remove(event);event.recycle();ticket.callback.accept(handled);}
    private HostWindowManager(){}
    public static boolean outputAttached(){return owner!=null;}
    /** Desktop XR controls belong only to the rendered Activity's own input root. */
    public static boolean xrInputFocused(Activity activity){
        check();if(owner!=activity||!outputFocused||inputOutput!=null||HostActivity.hostSleeping()||HostTextInput.active()||focusedRoot==null)return false;
        View decor=activity.getWindow().getDecorView();if(focusedRoot.getView()!=decor)return false;
        for(ViewRootImpl root:roots()){View view=root.getView();if(view!=null&&view!=decor&&view.isAttachedToWindow()&&view.getVisibility()==View.VISIBLE&&root.mSurface.isValid()&&(root.mWindowAttributes.flags&WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE)==0)return false;}
        return true;
    }
    static boolean ownsOutput(Activity activity){return owner==activity;}
    static boolean canOrientOutput(Activity activity){return ownsOutput(activity)&&(orientationOutput!=null||maclauncher.android.bridge.WindowSession.attached());}
    static void requestOrientation(Activity activity,boolean landscape){check();if(!ownsOutput(activity))throw new IllegalStateException("Orientation requires the attached Activity");if(orientationOutput!=null)orientationOutput.request(activity,landscape);else maclauncher.android.bridge.WindowSession.hostRequestOrientation(activity,landscape);}
    public static void install(Context context){install(context,false);}
    public static HostWindowService install(Context context,boolean originalHardwareRenderer){
        if(service==null){service=new HostWindowService(context,originalHardwareRenderer);service.install();}
        HostInputManager.install();HostInputMethodService.install();return service;
    }
    private static void check(){if(!android.os.Looper.getMainLooper().isCurrentThread())throw new IllegalStateException("Windows require the main window thread");}
    /** Physical input uses the shared ingress; retain the selected renderer. */
    public static void prepareView(ViewRootImpl root,WindowManager.LayoutParams attributes){attributes.inputFeatures|=WindowManager.LayoutParams.INPUT_FEATURE_NO_INPUT_CHANNEL;if(service==null||!service.usesOriginalHardwareRenderer())attributes.flags&=~WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;}
    public static void viewAttached(ViewRootImpl root){
        var callback=(InputQueue.Callback)HostUiAccess.get(root,ViewRootImpl.class,"mInputQueueCallback");
        if(callback!=null&&HostUiAccess.get(root,ViewRootImpl.class,"mInputQueue")==null){InputQueue queue=new InputQueue();HostUiAccess.set(root,ViewRootImpl.class,"mInputQueue",queue);callback.onInputQueueCreated(queue);}
        HostInputManager.attach(root,root.getWindowToken());
        View view=root.getView();
        if(view!=null)com.android.internal.view.OneShotPreDrawListener.add(view,()->{
            if(view.getViewRootImpl()==root)updateFocus(null);
        });
        updateFocus(null);
    }
    public static void viewDetached(ViewRootImpl root){HostScrollInput.retire(root);if(touchRoot==root){HostInput.cancelTouches(false);touchRoot=null;}HostInputManager.detach(root);updateFocus(root);}
    private static ArrayList<View> views(){return WindowManagerGlobal.getInstance().getWindowViews();}
    private static List<ViewRootImpl> roots(){return owner==null?List.of():WindowManagerGlobal.getInstance().getRootViews(owner.getActivityToken());}
    static void activate(Activity activity){check();owner=Objects.requireNonNull(activity);HostActivity.activateOutput(activity);HostActivity.applyOrientation(activity);updateFocus(null);}
    private static void updateFocus(ViewRootImpl retiring){
        // Original ViewRoot creates its Surface and attaches the actual view
        // during traversal. Selecting it at setView return loses the focus
        // transition before native engines have their SurfaceView. The original
        // one-shot pre-draw listener performs the first eligible selection.
        ViewRootImpl next=null;if(outputFocused&&inputOutput==null&&!HostActivity.hostSleeping())for(ViewRootImpl root:roots()){View view=root.getView();if(root!=retiring&&view!=null&&view.isAttachedToWindow()&&root.mSurface.isValid()&&view.getVisibility()==View.VISIBLE&&(root.mWindowAttributes.flags&WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)==0)next=root;}
        if(next==focusedRoot)return;HostScrollInput.cancel();HostInput.cancelTouches(true);ViewRootImpl previous=focusedRoot;focusedRoot=next;
        if(previous!=null&&previous!=retiring)previous.windowFocusChanged(false);if(next!=null)next.windowFocusChanged(true);
    }
    public static void attachOutput(Activity activity,SurfaceControl output,int width,int height){
        attachOutput(activity,output,width,height,null);
    }
    public static void attachOutput(Activity activity,SurfaceControl output,int width,int height,OrientationOutput orientation){
        check();if(owner!=null)throw new IllegalStateException("Native window output is already attached");
        orientationOutput=orientation;
        owner=Objects.requireNonNull(activity);install(activity);service.attachOutput(output,width,height);
        Window window=activity.getWindow();View decor=window.getDecorView();
        if(decor.getParent()==null){
            // ActivityThread attaches the Window's own attributes. Later
            // setFlags/setAttributes calls reuse them through WindowManagerGlobal;
            // a separate copy loses the activity token on that first update.
            var attrs=window.getAttributes();attrs.type=WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
            attrs.token=activity.getActivityToken();activity.getWindowManager().addView(decor,attrs);
        }
        HostActivity.windowAttached(activity);
        HostActivity.applyOrientation(activity);
    }
    public static void resizeOutput(int width,int height){check();if(service!=null)service.resizeOutput(width,height);for(View view:views()){ViewRootImpl root=view.getViewRootImpl();if(root!=null){root.setLayoutParams(root.mWindowAttributes,false);root.requestLayout();}}}
    public static void detachOutput(){check();HostScrollInput.cancel();HostInput.cancelTouches(true);if(service!=null)service.detachOutput();outputFocused=false;updateFocus(null);owner=null;orientationOutput=null;}
    public static void closeActivity(Activity activity){check();for(ViewRootImpl root:WindowManagerGlobal.getInstance().getRootViews(activity.getActivityToken())){View view=root.getView();if(view!=null)WindowManagerGlobal.getInstance().removeView(view,true);}if(owner==activity){owner=null;updateFocus(null);}}
    public static View topView(Activity activity){check();if(owner!=null&&owner!=activity)return null;View top=null;for(View view:views())if(view.getVisibility()==View.VISIBLE)top=view;return top;}
    private static Rect inputFrame(ViewRootImpl root){try{return (Rect)windowFrame.get(root);}catch(IllegalAccessException failure){throw new IllegalStateException("Android window frame is unavailable",failure);}}
    static ViewRootImpl pointerTarget(float x,float y){
        ViewRootImpl target=null;
        for(View view:views()){
            ViewRootImpl root=view.getViewRootImpl();if(root==null||view.getVisibility()!=View.VISIBLE)continue;
            int flags=root.mWindowAttributes.flags;if((flags&WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE)!=0)continue;
            Rect frame=inputFrame(root);if(frame.isEmpty())continue;
            boolean inside=x>=frame.left&&y>=frame.top&&x<frame.right&&y<frame.bottom;
            if(inside||(flags&(WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL|WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE))==0)target=root;
        }return target;
    }
    public static boolean input(InputEvent event){return input(event,null);}
    public static boolean input(InputEvent event,java.util.function.Consumer<Boolean> completion){
        check();Objects.requireNonNull(event);ViewRootImpl target=null;
        if(HostActivity.hostSleeping()&&!(event instanceof MotionEvent motion&&motion.getActionMasked()==MotionEvent.ACTION_CANCEL)){event.recycle();if(completion!=null)completion.accept(false);return false;}
        if(inputOutput!=null){
            if(completion==null){try{return inputOutput.input(event,null);}finally{event.recycle();}}
            InputCompletion ticket=new InputCompletion(completion,inputOutput,event.getSequenceNumber());inputCompletions.put(event,ticket);
            try{boolean accepted=inputOutput.input(event,handled->completeDelegated(event,ticket,handled));if(!accepted)completeDelegated(event,ticket,false);return accepted;}
            catch(RuntimeException|Error failure){completeDelegated(event,ticket,false);throw failure;}
        }
        if(event instanceof KeyEvent key&&(HostSensors.key(key)||HostInput.keyboardTouch(key))){event.recycle();if(completion!=null)completion.accept(true);return true;}
        if(event instanceof MotionEvent motion&&(motion.getSource()&InputDevice.SOURCE_CLASS_POINTER)!=0&&!motion.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)){
            int action=motion.getActionMasked();
            if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_SCROLL){
                target=pointerTarget(motion.getX(),motion.getY());
                if(action==MotionEvent.ACTION_DOWN)touchRoot=target;
            }else target=touchRoot;
            // InputDispatcher normally removes the physical window origin.
            // A decor's screen location also contains its current scroll/pan;
            // original ViewRoot applies that scroll and compatibility scale.
            if(target!=null){Rect frame=inputFrame(target);motion.offsetLocation(-frame.left,-frame.top);}
            if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)touchRoot=null;
        }else target=focusedRoot;
        if(target==null){event.recycle();if(completion!=null)completion.accept(false);return false;}
        if(completion!=null)inputCompletions.put(event,new InputCompletion(completion,null,event.getSequenceNumber()));
        target.dispatchInputEvent(event);return true;
    }
    public static void inputFinished(InputOutput output,int sequence,boolean handled){check();for(var entry:List.copyOf(inputCompletions.entrySet()))if(entry.getValue().output==output&&entry.getValue().sequence==sequence){completeDelegated(entry.getKey(),entry.getValue(),handled);return;}}
    public static void inputFinished(InputEvent event,boolean handled){var completion=inputCompletions.remove(event);if(completion!=null)completion.callback.accept(handled);}
    public static void inputFinished(InputEvent event,int flags,int handledMask){inputFinished(event,(flags&handledMask)!=0);}
    static void forgetInputCompletion(InputEvent event){var ticket=inputCompletions.remove(event);if(ticket!=null&&ticket.output!=null)event.recycle();}
    public static View focusedView(){check();return focusedRoot==null||focusedRoot.getView()==null?null:focusedRoot.getView().findFocus();}
    public static boolean outputFocused(){return outputFocused;}
    public static void focusOutput(boolean focused){check();if(HostActivity.hostSleeping())return;outputFocused=focused;if(!focused){HostScrollInput.cancel();HostInput.cancelTouches(true);HostSensors.reset();}updateFocus(null);HostActivity.outputStateChanged();}
    static void hostSleepChanged(){check();if(HostActivity.hostSleeping()){HostScrollInput.cancel();HostInput.cancelTouches(true);HostSensors.reset();}updateFocus(null);HostActivity.outputStateChanged();}
    public static void detachPresentation(){check();for(View view:views())WindowManagerGlobal.getInstance().removeView(view,true);detachOutput();}
    public static void shutdown(){check();Throwable failure=null;try{detachPresentation();}catch(Throwable error){failure=error;}if(service!=null){try{service.close();}catch(Throwable error){if(failure==null)failure=error;else if(failure!=error)failure.addSuppressed(error);}service=null;}HostInputManager.shutdown();HostInputMethodService.shutdown();if(failure!=null)HostUiAccess.raise(failure);}
}
