package maclauncher.android.framework;

import android.os.*;
import android.view.*;

/** Wheel/trackpad ingress. Original scroll handling wins; touch-only surfaces
 * receive one continuous gesture through the same original ViewRoot pipeline. */
final class HostScrollInput {
    private static final long QUIET_MILLIS=140;
    private static Handler handler;
    private static ViewRootImpl target;
    private static MotionEvent pending;
    private static int mode; // 0: first original scroll, 1: original scroll, 2: touch
    private static boolean posted,touch,allowTouch;
    private static float cursorX,cursorY,x,y,deltaX,deltaY;
    private static long lastInput,generation;
    private static final Runnable pump=HostScrollInput::pump,finish=HostScrollInput::finish;
    private HostScrollInput(){}

    static void scroll(float px,float py,float dx,float dy,boolean touchAllowed){
        if(!Float.isFinite(px)||!Float.isFinite(py)||!Float.isFinite(dx)||!Float.isFinite(dy)||(dx==0&&dy==0))return;
        ViewRootImpl selected=HostWindowManager.pointerTarget(px,py);long now=SystemClock.uptimeMillis();
        if(selected!=target||pending==null&&now-lastInput>QUIET_MILLIS)cancel();
        if(selected==null)return;
        if(handler==null)handler=Handler.createAsync(Looper.getMainLooper());
        if(target==null){target=selected;cursorX=x=px;cursorY=y=py;allowTouch=touchAllowed&&!HostInput.touchActive();}
        cursorX=px;cursorY=py;lastInput=now;deltaX+=dx;deltaY+=dy;
        schedule();idle();
    }
    private static void schedule(){if(!posted&&pending==null&&target!=null&&(deltaX!=0||deltaY!=0)){posted=handler.post(pump);}}
    private static void idle(){handler.removeCallbacks(finish);handler.postDelayed(finish,Math.max(0,lastInput+QUIET_MILLIS-SystemClock.uptimeMillis()));}
    private static void pump(){
        posted=false;if(target==null||pending!=null)return;
        View view=target.getView();if(view==null||view.getViewRootImpl()!=target){cancel();return;}
        float dx=deltaX,dy=deltaY;deltaX=deltaY=0;
        if(mode==2){
            if(allowTouch){
                if(!touch){HostWindowManager.input(HostInput.touch(MotionEvent.ACTION_DOWN,x,y));touch=true;}
                ViewConfiguration scale=ViewConfiguration.get(view.getContext());
                x-=dx*scale.getScaledHorizontalScrollFactor();y+=dy*scale.getScaledVerticalScrollFactor();
                long sequence=generation;pending=HostInput.touch(MotionEvent.ACTION_MOVE,x,y);
                HostWindowManager.input(pending,handled->{if(sequence!=generation)return;pending=null;schedule();idle();});
            }
            idle();return;
        }
        long sequence=generation;pending=HostInput.scrollEvent(cursorX,cursorY,dx,dy);
        HostWindowManager.input(pending,handled->{
            if(sequence!=generation)return;pending=null;
            if(mode==0){mode=handled||!allowTouch?1:2;if(mode==2){deltaX+=dx;deltaY+=dy;}}
            schedule();idle();
        });
    }
    private static void finish(){
        if(target==null||pending!=null||posted)return;
        if(SystemClock.uptimeMillis()-lastInput<QUIET_MILLIS) {idle();return;}
        if(touch)HostWindowManager.input(HostInput.touch(MotionEvent.ACTION_UP,x,y));
        clear();
    }
    static void retire(ViewRootImpl root){if(target==root)cancel();}
    static void cancel(){
        if(pending!=null)HostWindowManager.forgetInputCompletion(pending);
        if(touch)HostWindowManager.input(HostInput.touch(MotionEvent.ACTION_CANCEL,x,y));
        clear();
    }
    private static void clear(){
        if(handler!=null){handler.removeCallbacks(pump);handler.removeCallbacks(finish);}
        generation++;target=null;pending=null;mode=0;posted=touch=allowTouch=false;deltaX=deltaY=0;
    }
}
