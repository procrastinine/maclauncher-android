package maclauncher.test;

import android.os.*;
import android.view.*;
import java.lang.reflect.*;
import java.util.*;
import java.util.function.*;
import org.json.*;

/** Bounded observation of the original Window callback, shared by ART and JVM. */
final class WindowInputTrace {
    private static final int CAPACITY=128;
    private static final ArrayDeque<JSONObject> events=new ArrayDeque<>();
    private static final IdentityHashMap<Window,Owner> owners=new IdentityHashMap<>();
    private static volatile long until;
    private static volatile boolean motion;
    private static volatile int windowCount;
    private static int dropped;
    private static Handler timer;
    private static final Runnable expire=WindowInputTrace::stop;
    private static JSONObject json(Object... values){try{var out=new JSONObject();for(int i=0;i<values.length;i+=2)out.put(String.valueOf(values[i]),values[i+1]);return out;}catch(JSONException failure){throw new IllegalArgumentException(failure);}}
    static synchronized void record(JSONObject value){if(events.size()==CAPACITY){events.removeFirst();dropped++;}events.addLast(value);}
    static JSONObject capture(MotionEvent event){
        if(System.nanoTime()>=until)return null;
        int action=event.getActionMasked();if(action!=MotionEvent.ACTION_DOWN&&action!=MotionEvent.ACTION_UP&&action!=MotionEvent.ACTION_CANCEL&&!(motion&&action==MotionEvent.ACTION_MOVE))return null;
        return json("at",System.currentTimeMillis(),"action",action,"sequence",event.getSequenceNumber(),"ingress",AndroidTestControl.inputOrigin(event.getSequenceNumber()),
            "x",event.getX(),"y",event.getY(),"rawX",event.getRawX(),"rawY",event.getRawY(),
            "downTimeMs",event.getDownTime(),"eventTimeMs",event.getEventTime(),"deviceId",event.getDeviceId(),"source",event.getSource(),"displayId",event.getDisplayId(),
            "pointerCount",event.getPointerCount(),"handled",JSONObject.NULL,"finishedAt",JSONObject.NULL);
    }
    /** Every original callback method and exception passes through unchanged. */
    static Window.Callback observe(Window.Callback original,Function<MotionEvent,JSONObject> capture,Consumer<JSONObject> commit){
        return (Window.Callback)Proxy.newProxyInstance(Window.Callback.class.getClassLoader(),new Class<?>[]{Window.Callback.class},(proxy,method,args)->{
            JSONObject row=null;
            if(method.getName().equals("dispatchTouchEvent")&&args!=null&&args.length==1&&args[0] instanceof MotionEvent motion)
                try{row=capture.apply(motion);}catch(RuntimeException|LinkageError unavailable){}
            // Retain entry before calling the app: a blocked callback must
            // remain visible to the independent diagnostics lane.
            if(row!=null)try{commit.accept(row);}catch(RuntimeException unavailable){}
            Object result;
            try{result=method.invoke(original,args);}
            catch(InvocationTargetException failure){if(row!=null)synchronized(row){try{row.put("errorClass",failure.getCause().getClass().getName());row.put("finishedAt",System.currentTimeMillis());}catch(JSONException unavailable){}}throw failure.getCause();}
            if(row!=null)synchronized(row){try{row.put("handled",result);row.put("finishedAt",System.currentTimeMillis());}catch(JSONException unavailable){}}
            return result;
        });
    }
    private static final class Owner implements View.OnAttachStateChangeListener {
        final Window window;final View decor;final Window.Callback original,proxy;
        Owner(Window window,View decor){this.window=window;this.decor=decor;original=window.getCallback();proxy=observe(original,this::capture,WindowInputTrace::record);window.setCallback(proxy);decor.addOnAttachStateChangeListener(this);}
        JSONObject capture(MotionEvent motion){
            JSONObject row=WindowInputTrace.capture(motion);if(row==null)return null;
            int[] origin=new int[2];decor.getLocationOnScreen(origin);
            try{return row.put("windowId",System.identityHashCode(decor)).put("receiver",original.getClass().getName())
                .put("viewX",origin[0]).put("viewY",origin[1]).put("viewWidth",decor.getWidth()).put("viewHeight",decor.getHeight()).put("windowFocused",decor.hasWindowFocus());}
            catch(JSONException failure){throw new IllegalArgumentException(failure);}
        }
        void close(){if(window.getCallback()==proxy)window.setCallback(original);decor.removeOnAttachStateChangeListener(this);owners.remove(window);windowCount=owners.size();}
        @Override public void onViewAttachedToWindow(View view){}
        @Override public void onViewDetachedFromWindow(View view){close();}
    }
    private static Window window(View decor){
        for(Class<?> type=decor.getClass();type!=null;type=type.getSuperclass())try{
            Field field=type.getDeclaredField("mWindow");field.setAccessible(true);Object value=field.get(decor);return value instanceof Window window?window:null;
        }catch(NoSuchFieldException missing){}catch(ReflectiveOperationException|RuntimeException unavailable){return null;}
        return null;
    }
    private static void stop(){until=0;if(timer!=null)timer.removeCallbacks(expire);for(Owner owner:List.copyOf(owners.values()))owner.close();}
    static JSONObject control(JSONObject request)throws JSONException {
        if(!Looper.getMainLooper().isCurrentThread())throw new IllegalStateException("Window observation requires the Android main queue");
        if(request.optBoolean("start")){
            int seconds=request.optInt("seconds",10);if(seconds<1||seconds>60)throw new IllegalArgumentException("Window input trace duration must be 1–60 seconds");
            stop();synchronized(WindowInputTrace.class){events.clear();dropped=0;}motion=request.optBoolean("motion");until=System.nanoTime()+seconds*1_000_000_000L;
            for(View view:WindowManagerGlobal.getInstance().getWindowViews())if(!request.has("windowId")||System.identityHashCode(view)==request.getInt("windowId")){
                Window window=window(view);if(window!=null&&window.getCallback()!=null&&!owners.containsKey(window))owners.put(window,new Owner(window,view));
            }
            windowCount=owners.size();
            timer=Handler.createAsync(Looper.getMainLooper());timer.postDelayed(expire,seconds*1000L);
        }
        if(request.optBoolean("stop"))stop();
        if(until!=0&&System.nanoTime()>=until)stop();
        return snapshot();
    }
    static synchronized JSONObject snapshot()throws JSONException {
        var retained=new JSONArray();for(JSONObject row:events)synchronized(row){retained.put(new JSONObject(row.toString()));}
        boolean active=until!=0&&System.nanoTime()<until;
        return json("scope","original-window-touch-callback","active",active,"restorePending",until!=0&&!active,"motion",motion,"capacity",CAPACITY,"dropped",dropped,"windows",windowCount,"events",retained);
    }
}
