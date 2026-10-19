package maclauncher.android.media;

import android.media.*;
import java.lang.reflect.*;

/** Builds upstream audio descriptors from the existing CoreAudio route snapshot. */
public final class HostAudioDevices {
    private HostAudioDevices(){}
    private static final Constructor<?> handle,port;
    static{try{
        Class<?> handleClass=Class.forName("android.media.AudioHandle");
        handle=handleClass.getDeclaredConstructor(int.class);
        port=AudioDevicePort.class.getDeclaredConstructor(handleClass,String.class,int[].class,int[].class,int[].class,int[].class,AudioGain[].class,int.class,String.class,int[].class,int[].class);
        handle.setAccessible(true);port.setAccessible(true);
    }catch(ReflectiveOperationException failure){throw new ExceptionInInitializerError(failure);}}
    public static AudioDevicePort outputPort(int id,int type,String name,String address,int rate,int channels){
        if(id<=0||rate<=0||channels<=0||channels>32||name==null||address==null)
            throw new IllegalArgumentException("Invalid native audio route");
        int indexMask=channels==32?-1:(1<<channels)-1;
        try{return (AudioDevicePort)port.newInstance(handle.newInstance(id),name,new int[]{rate},new int[0],new int[]{indexMask},new int[]{AudioFormat.ENCODING_PCM_16BIT,AudioFormat.ENCODING_PCM_FLOAT},new AudioGain[0],AudioDeviceInfo.convertDeviceTypeToInternalDevice(type),address,new int[0],new int[0]);}
        catch(InvocationTargetException failure){Throwable cause=failure.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}
        catch(ReflectiveOperationException failure){throw new IllegalStateException("Audio descriptor construction failed",failure);}
    }
    /** Retire the original Android port dispatcher with its native route monitor. */
    public static void shutdown(){
        try{
            Field field=AudioManager.class.getDeclaredField("sAudioPortEventHandler");field.setAccessible(true);Object owner=field.get(null);
            Field thread=owner.getClass().getDeclaredField("mHandlerThread");thread.setAccessible(true);if(thread.get(owner)==null)return;
            Method finalize=owner.getClass().getDeclaredMethod("finalize");finalize.setAccessible(true);finalize.invoke(owner);
        }catch(InvocationTargetException failure){Throwable cause=failure.getCause();if(cause instanceof RuntimeException e)throw e;if(cause instanceof Error e)throw e;throw new IllegalStateException(cause);}
        catch(ReflectiveOperationException failure){throw new IllegalStateException("Audio dispatcher shutdown failed",failure);}
    }
}
