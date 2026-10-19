package maclauncher.android.framework;

import android.graphics.Bitmap;
import android.os.*;
import android.view.*;
import java.lang.reflect.Field;
import java.util.*;

/** SDL event and cursor ingress into the original Android input objects. */
public final class HostInput {
    private HostInput(){}
    private static final Field hotX=field(PointerIcon.class,"mHotSpotX"),hotY=field(PointerIcon.class,"mHotSpotY");
    // All native/test ingress runs on the original Android window thread. Mouse,
    // wheel gestures and keyboard contacts belong to its one touchscreen stream.
    private static final Stream stream=new Stream();
    private static String joystickKeys="wasd";
    private static float joystickX=.18f,joystickY=.75f,joystickRadius=.12f;
    private static final Set<Integer> heldMovement=new HashSet<>();
    private static int pointerId,pointerGeneration,pointerWidth,pointerHeight;
    // SDL's standardized controller layout; the native owner reports which controls exist.
    private static final int[] GAMEPAD_KEYS={KeyEvent.KEYCODE_BUTTON_A,KeyEvent.KEYCODE_BUTTON_B,KeyEvent.KEYCODE_BUTTON_X,KeyEvent.KEYCODE_BUTTON_Y,
        KeyEvent.KEYCODE_BUTTON_SELECT,KeyEvent.KEYCODE_BUTTON_MODE,KeyEvent.KEYCODE_BUTTON_START,KeyEvent.KEYCODE_BUTTON_THUMBL,KeyEvent.KEYCODE_BUTTON_THUMBR,
        KeyEvent.KEYCODE_BUTTON_L1,KeyEvent.KEYCODE_BUTTON_R1,KeyEvent.KEYCODE_DPAD_UP,KeyEvent.KEYCODE_DPAD_DOWN,KeyEvent.KEYCODE_DPAD_LEFT,KeyEvent.KEYCODE_DPAD_RIGHT,
        KeyEvent.KEYCODE_BUTTON_1,KeyEvent.KEYCODE_BUTTON_2,KeyEvent.KEYCODE_BUTTON_3,KeyEvent.KEYCODE_BUTTON_4,KeyEvent.KEYCODE_BUTTON_5,KeyEvent.KEYCODE_BUTTON_6,
        KeyEvent.KEYCODE_BUTTON_7,KeyEvent.KEYCODE_BUTTON_8,KeyEvent.KEYCODE_BUTTON_9,KeyEvent.KEYCODE_BUTTON_10,KeyEvent.KEYCODE_BUTTON_11};
    private static final int[] GAMEPAD_AXES={MotionEvent.AXIS_X,MotionEvent.AXIS_Y,MotionEvent.AXIS_Z,MotionEvent.AXIS_RZ,MotionEvent.AXIS_LTRIGGER,MotionEvent.AXIS_RTRIGGER};
    // Confined to the native window's SDL event thread, like the touch stream above.
    private static final Map<Long,Gamepad> gamepads=new LinkedHashMap<>();
    private static final class Gamepad {
        final int id,generation;final boolean[] buttons=new boolean[GAMEPAD_KEYS.length],axes=new boolean[GAMEPAD_AXES.length];
        final long[] pressed=new long[GAMEPAD_KEYS.length];final float[] values=new float[GAMEPAD_AXES.length];
        final MotionEvent.PointerProperties[] properties={new MotionEvent.PointerProperties()};
        final MotionEvent.PointerCoords[] coordinates={new MotionEvent.PointerCoords()};
        final boolean hatX,hatY;
        Gamepad(int id,int generation,int[] buttons,int[] axes){
            this.id=id;this.generation=generation;for(int button:buttons)if(button>=0&&button<this.buttons.length)this.buttons[button]=true;
            for(int axis:axes)if(axis>=0&&axis<this.axes.length)this.axes[axis]=true;
            hatX=this.buttons[13]||this.buttons[14];hatY=this.buttons[11]||this.buttons[12];
            properties[0].id=0;properties[0].toolType=MotionEvent.TOOL_TYPE_UNKNOWN;
        }
        void motion(){
            MotionEvent.PointerCoords coords=coordinates[0];coords.clear();for(int axis=0;axis<axes.length;axis++)if(axes[axis])coords.setAxisValue(GAMEPAD_AXES[axis],values[axis]);
            if(hatX)coords.setAxisValue(MotionEvent.AXIS_HAT_X,(pressed[14]!=0?1:0)-(pressed[13]!=0?1:0));
            if(hatY)coords.setAxisValue(MotionEvent.AXIS_HAT_Y,(pressed[12]!=0?1:0)-(pressed[11]!=0?1:0));
            long now=SystemClock.uptimeMillis();HostWindowManager.input(MotionEvent.obtain(now,now,MotionEvent.ACTION_MOVE,1,properties,coordinates,0,0,1,1,id,0,InputDevice.SOURCE_JOYSTICK,0));
        }
        void button(int button,boolean down,boolean cancelled){
            if(button<0||button>=buttons.length||!buttons[button]||(pressed[button]!=0)==down)return;
            long now=SystemClock.uptimeMillis(),start=down?Math.max(1,now):pressed[button];pressed[button]=down?start:0;
            HostWindowManager.input(new KeyEvent(start,now,down?KeyEvent.ACTION_DOWN:KeyEvent.ACTION_UP,GAMEPAD_KEYS[button],0,0,id,button,cancelled?KeyEvent.FLAG_CANCELED:0,InputDevice.SOURCE_GAMEPAD));
            if(button>=11&&button<=14)motion();
        }
        void reset(){boolean changed=false;for(int button=0;button<pressed.length;button++)if(pressed[button]!=0)button(button,false,true);for(int axis=0;axis<values.length;axis++)if(values[axis]!=0){values[axis]=0;changed=true;}if(changed)motion();HostSensors.controllerTilt(id,0,0);}
    }
    /** Called only for a successfully opened SDL controller, including mapping changes. */
    public static void gamepadAdded(long instance,String name,String descriptor,int vendor,int product,int[] buttons,int[] axes){
        Gamepad prior=gamepads.get(instance);if(prior!=null)prior.reset();
        Gamepad pad=new Gamepad(prior==null?HostInputManager.allocateDeviceId():prior.id,prior==null?1:Math.incrementExact(prior.generation),buttons,axes);
        int sources=InputDevice.SOURCE_GAMEPAD;boolean joystick=pad.hatX||pad.hatY;for(boolean axis:pad.axes)joystick|=axis;
        if(joystick)sources|=InputDevice.SOURCE_JOYSTICK;if(pad.hatX||pad.hatY)sources|=InputDevice.SOURCE_DPAD;
        InputDevice.Builder builder=new InputDevice.Builder().setId(pad.id).setGeneration(pad.generation).setControllerNumber(pad.id)
            .setName(Objects.requireNonNull(name)).setDescriptor(Objects.requireNonNull(descriptor)).setVendorId(vendor).setProductId(product)
            .setExternal(true).setSources(sources).setKeyboardType(InputDevice.KEYBOARD_TYPE_NON_ALPHABETIC).setKeyCharacterMap(KeyCharacterMap.obtainEmptyMap(pad.id)).setEnabled(true);
        Set<Integer> keys=new HashSet<>();for(int button=0;button<pad.buttons.length;button++)if(pad.buttons[button])keys.add(GAMEPAD_KEYS[button]);
        for(int axis=0;axis<pad.axes.length;axis++)if(pad.axes[axis])builder.addMotionRange(GAMEPAD_AXES[axis],InputDevice.SOURCE_JOYSTICK,axis>=4?0:-1,1,0,0,axis>=4?1f/32767:1f/32768);
        if(pad.hatX)builder.addMotionRange(MotionEvent.AXIS_HAT_X,InputDevice.SOURCE_JOYSTICK,-1,1,0,0,1);
        if(pad.hatY)builder.addMotionRange(MotionEvent.AXIS_HAT_Y,InputDevice.SOURCE_JOYSTICK,-1,1,0,0,1);
        InputDevice device=builder.build();gamepads.put(instance,pad);HostInputManager.installGamepad(device,keys);
    }
    public static void gamepadRemoved(long instance){Gamepad pad=gamepads.remove(instance);if(pad!=null)try{pad.reset();}finally{HostInputManager.removeGamepad(pad.id);}}
    public static void gamepadReset(long instance){Gamepad pad=gamepads.get(instance);if(pad!=null)pad.reset();}
    public static void gamepadButton(long instance,int button,boolean down){Gamepad pad=gamepads.get(instance);if(pad!=null)pad.button(button,down,false);}
    public static void gamepadAxis(long instance,int axis,int value){
        Gamepad pad=gamepads.get(instance);if(pad==null||axis<0||axis>=pad.axes.length||!pad.axes[axis])return;
        float normalized=value<0?value/32768f:value/32767f;if(pad.values[axis]==normalized)return;pad.values[axis]=normalized;pad.motion();
        if(axis==2||axis==3)HostSensors.controllerTilt(pad.id,pad.values[2],pad.values[3]);
    }
    private static final class Contact {
        final MotionEvent.PointerProperties property=new MotionEvent.PointerProperties();
        final MotionEvent.PointerCoords position=new MotionEvent.PointerCoords();
        Contact(int id){property.id=id;property.toolType=MotionEvent.TOOL_TYPE_FINGER;position.size=1;}
    }
    private static final class Stream {
        long down;float x,y;
        final Contact mouse=new Contact(0),joystick=new Contact(1);
        final ArrayList<Contact> active=new ArrayList<>(2);
        final MotionEvent.PointerProperties[][] properties={null,new MotionEvent.PointerProperties[1],new MotionEvent.PointerProperties[2]};
        final MotionEvent.PointerCoords[][] coordinates={null,new MotionEvent.PointerCoords[1],new MotionEvent.PointerCoords[2]};
        MotionEvent event(int action,long now){
            int count=active.size();for(int i=0;i<count;i++){properties[count][i]=active.get(i).property;coordinates[count][i]=active.get(i).position;}
            return MotionEvent.obtain(down,now,action,count,properties[count],coordinates[count],0,0,1,1,pointerId,0,InputDevice.SOURCE_TOUCHSCREEN,0);
        }
        MotionEvent change(Contact contact,int action,float px,float py){
            if(!Float.isFinite(px)||!Float.isFinite(py))throw new IllegalArgumentException("Pointer coordinates are not finite");
            long now=SystemClock.uptimeMillis();int index=active.indexOf(contact);
            contact.position.x=px;contact.position.y=py;
            if(action==MotionEvent.ACTION_CANCEL){
                if(active.isEmpty()){down=now;active.add(contact);}
                for(Contact value:active)value.position.pressure=0;
                MotionEvent result=event(action,now);active.clear();heldMovement.clear();return result;
            }
            if(index<0){
                if(action!=MotionEvent.ACTION_DOWN&&!active.isEmpty())return event(MotionEvent.ACTION_MOVE,now);
                if(active.isEmpty())down=now;active.add(contact);index=active.size()-1;
                // Native mouse drags can resume after focus was lost. Retain
                // their original event without creating a stuck owned contact.
                if(action!=MotionEvent.ACTION_DOWN){contact.position.pressure=action==MotionEvent.ACTION_UP?0:1;MotionEvent result=event(action,now);active.remove(index);return result;}
                action=active.size()==1?MotionEvent.ACTION_DOWN:MotionEvent.ACTION_POINTER_DOWN|(index<<MotionEvent.ACTION_POINTER_INDEX_SHIFT);
            }else if(action==MotionEvent.ACTION_DOWN)action=MotionEvent.ACTION_MOVE;
            boolean release=action==MotionEvent.ACTION_UP;
            contact.position.pressure=release?0:1;
            if(release&&active.size()>1)action=MotionEvent.ACTION_POINTER_UP|(index<<MotionEvent.ACTION_POINTER_INDEX_SHIFT);
            MotionEvent result=event(action,now);if(release)active.remove(index);return result;
        }
    }
    private static Field field(Class<?> type,String name){try{Field field=type.getDeclaredField(name);field.setAccessible(true);return field;}catch(ReflectiveOperationException failure){throw new ExceptionInInitializerError(failure);}}
    private static native KeyCharacterMap loadKeyboardMap(String library,String contents);
    @FunctionalInterface public interface KeyboardLoader {KeyCharacterMap load(String contents);}
    private static KeyboardLoader keyboardLoader=text->loadKeyboardMap(JvmNativeMemory.nativeLibrary(),text);
    private static boolean keyboardInitialized;
    /** The VM supplies its original native parser; device construction stays shared. */
    public static void keyboardLoader(KeyboardLoader loader){if(keyboardInitialized)throw new IllegalStateException("Android keyboard is already initialized");keyboardLoader=Objects.requireNonNull(loader);}
    public static void initialize(){
        KeyCharacterMap map;
        try(var input=HostInput.class.getResourceAsStream("/maclauncher/android/framework/Virtual.kcm")){
            if(input==null)throw new IllegalStateException("Android virtual keyboard map is unavailable");
            String text=new String(input.readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
            map=Objects.requireNonNull(keyboardLoader.load(text));
        }catch(java.io.IOException failure){throw new IllegalStateException("Android keyboard map could not be loaded",failure);}
        InputDevice device=new InputDevice.Builder().setId(-1).setGeneration(1).setName("Mac keyboard").setDescriptor("maclauncher:sdl-keyboard").setSources(InputDevice.SOURCE_KEYBOARD).setKeyboardType(InputDevice.KEYBOARD_TYPE_ALPHABETIC).setKeyCharacterMap(map).setEnabled(true).build();
        HostInputManager.installKeyboard(device);
        keyboardInitialized=true;
        var metrics=new android.util.DisplayMetrics();HostDisplay.metrics(metrics);pointerId=HostInputManager.allocateDeviceId();pointerGeneration=0;viewportChanged(metrics.widthPixels,metrics.heightPixels);
    }
    /** The adapted pointer is a discoverable device, separate from Android's virtual keyboard. */
    static synchronized void viewportChanged(int width,int height){
        if(pointerId==0||pointerGeneration!=0&&pointerWidth==width&&pointerHeight==height)return;
        pointerWidth=width;pointerHeight=height;pointerGeneration++;
        InputDevice device=new InputDevice.Builder().setId(pointerId).setGeneration(pointerGeneration).setName("Mac pointer").setDescriptor("maclauncher:sdl-pointer")
            .setSources(InputDevice.SOURCE_TOUCHSCREEN|InputDevice.SOURCE_MOUSE|InputDevice.SOURCE_MOUSE_RELATIVE).setKeyboardType(InputDevice.KEYBOARD_TYPE_NONE)
            .setKeyCharacterMap(KeyCharacterMap.obtainEmptyMap(pointerId)).setAssociatedDisplayId(Display.DEFAULT_DISPLAY).setEnabled(true)
            .addMotionRange(MotionEvent.AXIS_X,InputDevice.SOURCE_TOUCHSCREEN,0,Math.max(0,width-1),0,0,1)
            .addMotionRange(MotionEvent.AXIS_Y,InputDevice.SOURCE_TOUCHSCREEN,0,Math.max(0,height-1),0,0,1)
            .addMotionRange(MotionEvent.AXIS_PRESSURE,InputDevice.SOURCE_TOUCHSCREEN,0,1,0,0,0)
            .addMotionRange(MotionEvent.AXIS_X,InputDevice.SOURCE_MOUSE,0,Math.max(0,width-1),0,0,1)
            .addMotionRange(MotionEvent.AXIS_Y,InputDevice.SOURCE_MOUSE,0,Math.max(0,height-1),0,0,1)
            .addMotionRange(MotionEvent.AXIS_HSCROLL,InputDevice.SOURCE_MOUSE,-1,1,0,0,0)
            .addMotionRange(MotionEvent.AXIS_VSCROLL,InputDevice.SOURCE_MOUSE,-1,1,0,0,0)
            .addMotionRange(MotionEvent.AXIS_SIZE,InputDevice.SOURCE_TOUCHSCREEN,0,1,0,0,0).build();
        HostInputManager.installDevice(device,Set.of());
    }
    public static MotionEvent motion(int action,float x,float y){
        if(action==MotionEvent.ACTION_DOWN)HostScrollInput.cancel();
        return touch(action,x,y);
    }
    static boolean touchActive(){return !stream.active.isEmpty();}
    static MotionEvent touch(int action,float x,float y){
        stream.x=x;stream.y=y;return stream.change(stream.mouse,action,x,y);
    }
    /** Per-game normalized controls; physical mouse input retains its own finger. */
    public static void configureTouchJoystick(String keys,float x,float y,float radius){
        if(!Set.of("off","wasd","arrows","wasd-arrows").contains(keys)||!Float.isFinite(x)||!Float.isFinite(y)||!Float.isFinite(radius)||x<0||x>1||y<0||y>1||radius<=0||radius>.5f)throw new IllegalArgumentException("Invalid keyboard touch joystick");
        resetJoystick();joystickKeys=keys;joystickX=x;joystickY=y;joystickRadius=radius;
    }
    private static int direction(int code){
        boolean wasd=joystickKeys.equals("wasd")||joystickKeys.equals("wasd-arrows"),arrows=joystickKeys.equals("arrows")||joystickKeys.equals("wasd-arrows");
        if(wasd)return switch(code){case KeyEvent.KEYCODE_W->1;case KeyEvent.KEYCODE_S->2;case KeyEvent.KEYCODE_A->3;case KeyEvent.KEYCODE_D->4;default->arrows?arrowDirection(code):0;};
        return arrows?arrowDirection(code):0;
    }
    private static int arrowDirection(int code){return switch(code){case KeyEvent.KEYCODE_DPAD_UP->1;case KeyEvent.KEYCODE_DPAD_DOWN->2;case KeyEvent.KEYCODE_DPAD_LEFT->3;case KeyEvent.KEYCODE_DPAD_RIGHT->4;default->0;};}
    /** Keyboard game controls never consume keys owned by an Android editor. */
    static boolean keyboardControlsAllowed(){
        View focus=HostWindowManager.focusedView();
        return (focus==null||!focus.onCheckIsTextEditor())&&!HostTextInput.active();
    }
    static boolean keyboardTouch(KeyEvent event){
        if(joystickKeys.equals("off"))return false;
        if(!keyboardControlsAllowed()){resetJoystick();return false;}
        int code=event.getKeyCode(),direction=direction(code);boolean held=heldMovement.contains(code);
        if(direction==0||!held&&(event.isAltPressed()||event.isCtrlPressed()||event.isMetaPressed())||event.getAction()!=KeyEvent.ACTION_DOWN&&event.getAction()!=KeyEvent.ACTION_UP)return false;
        if(event.getAction()==KeyEvent.ACTION_UP&&!held)return false;
        boolean changed=event.getAction()==KeyEvent.ACTION_DOWN?heldMovement.add(code):heldMovement.remove(code);
        if(!changed)return true;
        boolean[] directions=new boolean[5];for(int key:heldMovement)directions[direction(key)]=true;
        int dx=(directions[4]?1:0)-(directions[3]?1:0),dy=(directions[2]?1:0)-(directions[1]?1:0);
        float x=joystickX*Math.max(0,pointerWidth-1),y=joystickY*Math.max(0,pointerHeight-1);
        if(heldMovement.isEmpty()){resetJoystick();return true;}
        if(!stream.active.contains(stream.joystick)){HostScrollInput.cancel();HostWindowManager.input(stream.change(stream.joystick,MotionEvent.ACTION_DOWN,x,y));}
        float length=(float)Math.hypot(dx,dy),radius=joystickRadius*Math.min(pointerWidth,pointerHeight);
        if(length>0){x+=dx/length*radius;y+=dy/length*radius;}
        x=Math.max(0,Math.min(Math.max(0,pointerWidth-1),x));y=Math.max(0,Math.min(Math.max(0,pointerHeight-1),y));
        HostWindowManager.input(stream.change(stream.joystick,MotionEvent.ACTION_MOVE,x,y));return true;
    }
    /** Text/focus/lifecycle owners retire controls without suppressing editor keys. */
    static void resetJoystick(){heldMovement.clear();if(stream.active.contains(stream.joystick))HostWindowManager.input(stream.change(stream.joystick,MotionEvent.ACTION_UP,stream.joystick.position.x,stream.joystick.position.y));}
    static void cancelTouches(boolean dispatch){
        heldMovement.clear();if(stream.active.isEmpty())return;
        if(dispatch)HostWindowManager.input(stream.change(stream.active.getFirst(),MotionEvent.ACTION_CANCEL,stream.active.getFirst().position.x,stream.active.getFirst().position.y));
        else stream.active.clear();
    }
    public static void wheel(float x,float y,float horizontal,float vertical,boolean allowTouch){if(!HostWindowManager.delegatedWheel(x,y,horizontal,vertical,allowTouch))HostScrollInput.scroll(x,y,horizontal,vertical,allowTouch);}
    static MotionEvent scrollEvent(float x,float y,float horizontal,float vertical){
        long now=SystemClock.uptimeMillis();var property=new MotionEvent.PointerProperties();property.id=0;property.toolType=MotionEvent.TOOL_TYPE_MOUSE;
        var coords=new MotionEvent.PointerCoords();coords.x=x;coords.y=y;coords.setAxisValue(MotionEvent.AXIS_HSCROLL,horizontal);coords.setAxisValue(MotionEvent.AXIS_VSCROLL,vertical);
        return MotionEvent.obtain(now,now,MotionEvent.ACTION_SCROLL,1,new MotionEvent.PointerProperties[]{property},new MotionEvent.PointerCoords[]{coords},0,0,1,1,pointerId,0,InputDevice.SOURCE_MOUSE,0);
    }
    public static MotionEvent relative(int action,float x,float y,int buttons){
        long now=SystemClock.uptimeMillis();var property=new MotionEvent.PointerProperties();property.id=0;property.toolType=MotionEvent.TOOL_TYPE_MOUSE;
        var coords=new MotionEvent.PointerCoords();coords.x=x;coords.y=y;coords.pressure=buttons==0?0:1;
        return MotionEvent.obtain(now,now,action,1,new MotionEvent.PointerProperties[]{property},new MotionEvent.PointerCoords[]{coords},0,buttons,1,1,pointerId,0,InputDevice.SOURCE_MOUSE_RELATIVE,0);
    }
    public static KeyEvent key(int action,int code,int unicode,int meta){long now=SystemClock.uptimeMillis();return new KeyEvent(now,now,action,code,0,meta,-1,0,0,InputDevice.SOURCE_KEYBOARD);}
    /** Physical keys currently translated by input-key.h, independent of character labels. */
    static boolean keyboardHasKey(int code){return code>=KeyEvent.KEYCODE_A&&code<=KeyEvent.KEYCODE_Z||code>=KeyEvent.KEYCODE_0&&code<=KeyEvent.KEYCODE_9||switch(code){case KeyEvent.KEYCODE_DPAD_UP,KeyEvent.KEYCODE_DPAD_DOWN,KeyEvent.KEYCODE_DPAD_LEFT,KeyEvent.KEYCODE_DPAD_RIGHT,KeyEvent.KEYCODE_ENTER,KeyEvent.KEYCODE_ESCAPE,KeyEvent.KEYCODE_SPACE,KeyEvent.KEYCODE_DEL,KeyEvent.KEYCODE_TAB->true;default->false;};}
    public static void cancel(View root){HostScrollInput.cancel();if(stream.active.isEmpty()){heldMovement.clear();return;}MotionEvent event=stream.change(stream.active.getFirst(),MotionEvent.ACTION_CANCEL,stream.x,stream.y);try{root.dispatchTouchEvent(event);}finally{event.recycle();}}
    public static void recycle(InputEvent event){event.recycle();}
    public static int[] cursor(PointerIcon icon){
        Bitmap bitmap=Objects.requireNonNull(icon).getBitmap();if(bitmap==null)return null;
        int width=bitmap.getWidth(),height=bitmap.getHeight();if(width<1||height<1||width>1024||height>1024)throw new IllegalArgumentException("Cursor dimensions exceed 1024 pixels");
        try{int[] result=new int[4+width*height];result[0]=width;result[1]=height;result[2]=(int)hotX.getFloat(icon);result[3]=(int)hotY.getFloat(icon);bitmap.getPixels(result,4,width,0,0,width,height);return result;}catch(IllegalAccessException failure){throw new IllegalStateException(failure);}
    }
}
