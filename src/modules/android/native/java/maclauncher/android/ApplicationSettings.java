package maclauncher.android;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Set;
import java.util.function.Function;

/** Shared application settings, independent of the executing VM and JSON reader. */
public record ApplicationSettings(String touchJoystick,float joystickX,float joystickY,float joystickRadius,
                                  String manualTilt,String audioInputUid,String cameraInputUid) {
    public static ApplicationSettings read(Function<String,Object> values)throws IOException {
        Object role=values.apply("serviceProcess");
        String joystickDefault=role instanceof Number number&&number.intValue()==1?"off":"wasd";
        return new ApplicationSettings(choice(values,"touchJoystick",joystickDefault,Set.of("off","wasd","arrows","wasd-arrows")),
            position(values,"touchJoystickX",1800),position(values,"touchJoystickY",7500),position(values,"touchJoystickRadius",1200),
            choice(values,"manualTilt","off",Set.of("off","keyboard","controller")),input(values,"audioInputUid"),input(values,"cameraInputUid"));
    }
    private static String choice(Function<String,Object> values,String name,String fallback,Set<String> choices)throws IOException {
        Object value=values.apply(name);if(value==null)return fallback;
        if(!(value instanceof String text)||!choices.contains(text))throw new IOException("Invalid Android input control: "+name);
        return text;
    }
    private static float position(Function<String,Object> values,String name,int fallback)throws IOException {
        Object raw=values.apply(name);long value=fallback;boolean radius=name.equals("touchJoystickRadius");
        if(raw!=null){if(!(raw instanceof Number number)||number.doubleValue()!=number.longValue())throw new IOException("Invalid touch joystick position or radius");value=number.longValue();}
        if(value<(radius?1:0)||value>(radius?5000:10000))throw new IOException("Invalid touch joystick position or radius");return value/10000f;
    }
    private static String input(Function<String,Object> values,String name)throws IOException {
        Object value=values.apply(name);if(value==null)return null;
        if(!(value instanceof String uid)||uid.length()>512||uid.getBytes(StandardCharsets.UTF_8).length>2048||uid.codePoints().anyMatch(Character::isISOControl))throw new IOException("Invalid Android media input identity");
        return uid.isEmpty()?null:uid;
    }
    public float position(String name){return switch(name){case "touchJoystickX"->joystickX;case "touchJoystickY"->joystickY;case "touchJoystickRadius"->joystickRadius;default->throw new IllegalArgumentException("Unknown touch position");};}
    public String input(String name){return switch(name){case "audioInputUid"->audioInputUid;case "cameraInputUid"->cameraInputUid;default->throw new IllegalArgumentException("Unknown media input");};}
}
