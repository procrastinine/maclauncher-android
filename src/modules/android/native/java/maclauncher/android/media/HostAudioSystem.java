package maclauncher.android.media;

import android.media.*;
import android.media.audiopolicy.AudioProductStrategy;
import android.media.audiopolicy.AudioVolumeGroup;
import java.util.*;

/** Exact existing device JNI, with its library loaded before framework static format queries. */
public final class HostAudioSystem {
    private HostAudioSystem(){}
    static {AudioHost.initialize();}
    private static final java.util.concurrent.atomic.AtomicInteger playerIds=new java.util.concurrent.atomic.AtomicInteger();
    /** IDs belong to this process's original PlaybackActivityMonitor. */
    public static int newAudioPlayerId(){int previous=playerIds.getAndUpdate(value->value==Integer.MAX_VALUE?value:value+1);return previous==Integer.MAX_VALUE?AudioPlaybackConfiguration.PLAYER_PIID_INVALID:previous+1;}
    public static boolean isStreamActive(int stream,int inPastMs){return HostAudioService.streamActive(stream,inPastMs);}
    // There is no Android remote-submix output in the local CoreAudio mixer.
    public static boolean isStreamActiveRemotely(int stream,int inPastMs){return false;}
    public static native int native_getMaxChannelCount();
    public static native int native_getMaxSampleRate();
    public static native int native_getMinSampleRate();
    public static native int getPrimaryOutputSamplingRate();
    public static native int getPrimaryOutputFrameCount();
    public static native int getOutputLatency(int stream);
    public static native int getDeviceConnectionState(int device,String address);
    public static native int listAudioPorts(ArrayList<AudioPort> ports,int[] generation);
    public static native int listAudioPatches(ArrayList<AudioPatch> patches,int[] generation);
    public static native int native_list_audio_product_strategies(ArrayList<AudioProductStrategy> strategies);
    public static native int native_list_audio_volume_groups(ArrayList<AudioVolumeGroup> groups);
}
