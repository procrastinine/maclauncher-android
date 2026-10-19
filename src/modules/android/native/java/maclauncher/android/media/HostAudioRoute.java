package maclauncher.android.media;

import android.media.*;

/** Owned route subscription, backed by the original AudioManager/CoreAudio port dispatcher. */
final class HostAudioRoute implements AutoCloseable {
    private final AudioManager manager=new AudioManager();
    private final Runnable changed;
    private boolean enabled,closed;
    private final AudioManager.OnAudioPortUpdateListener listener=new AudioManager.OnAudioPortUpdateListener(){
        public void onAudioPortListUpdate(AudioPort[] ports){deliver();}
        public void onAudioPatchListUpdate(AudioPatch[] patches){deliver();}
        public void onServiceDied(){deliver();}
    };
    HostAudioRoute(Runnable changed){this.changed=changed;}
    private void deliver(){synchronized(this){if(!enabled||closed)return;}changed.run();}
    synchronized void enabled(boolean value){if(closed)return;if(value==enabled)return;enabled=value;if(value)manager.registerAudioPortUpdateListener(listener);else manager.unregisterAudioPortUpdateListener(listener);}
    static int[] devices(){AudioDeviceInfo[] values=AudioManager.getDevicesStatic(AudioManager.GET_DEVICES_OUTPUTS);return values.length==0?new int[0]:new int[]{values[0].getId()};}
    static boolean select(int id){if(id==0)return true;int[] devices=devices();return devices.length!=0&&devices[0]==id;}
    public synchronized void close(){if(closed)return;enabled(false);closed=true;}
}
