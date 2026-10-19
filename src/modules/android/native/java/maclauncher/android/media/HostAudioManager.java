package maclauncher.android.media;
import android.media.*;
import static android.media.AudioManager.*;
/** Application-local output gain and the actual CoreAudio default-output monitor. */
public final class HostAudioManager {
 // One application session, independent of the Mac's master volume. The
 // software output has sixteen linear gain steps and no hardware minimum.
 private static final Object volumeLock=new Object();
 private static final int[] volumes={15,15,15,15,15,15,15,15,15,15,15,15};
 private static final boolean[] muted=new boolean[volumes.length];
 private static int ringerMode=RINGER_MODE_NORMAL;
 // The software output has separate ring, notification and system streams.
 // Ringer muting composes with explicit stream muting instead of overwriting it.
 private static final int ringerStreams=(1<<STREAM_RING)|(1<<STREAM_NOTIFICATION)|(1<<STREAM_SYSTEM)|(1<<STREAM_SYSTEM_ENFORCED);
 public boolean isStreamAffectedByRingerMode(int stream){checkedStream(stream);return (ringerStreams&(1<<stream))!=0;}
 private static boolean effectiveMute(int stream){return muted[stream]||(ringerMode!=RINGER_MODE_NORMAL&&(ringerStreams&(1<<stream))!=0);}
 public static int checkedStream(int stream){if(stream<0||stream>=volumes.length)throw new IllegalArgumentException("Invalid audio stream type "+stream);return stream;}
 private static native void nativeStreamVolume(int stream,int index);
 private static native boolean nativeStreamActive(int stream,int pastMs);
 /** Query the executing Android audio owner, including native-only players. */
 public static boolean streamActive(int stream,int pastMs){return nativeStreamActive(stream,pastMs);}
 private static void applyVolume(int stream){nativeStreamVolume(stream,effectiveMute(stream)?0:volumes[stream]);}
 public int getStreamMinVolume(int stream){checkedStream(stream);return 0;}
 public int getStreamMaxVolume(int stream){checkedStream(stream);return 15;}
 public int getStreamVolume(int stream){checkedStream(stream);synchronized(volumeLock){return effectiveMute(stream)?0:volumes[stream];}}
 public int getLastAudibleStreamVolume(int stream){checkedStream(stream);synchronized(volumeLock){return volumes[stream];}}
 public boolean isStreamMute(int stream){checkedStream(stream);synchronized(volumeLock){return effectiveMute(stream);}}
 public int getRingerMode(){synchronized(volumeLock){return ringerMode;}}
 // Klepton's shared output mixer uses normal media routing. Ringer muting is
 // a stream-gain policy, not a telephony or voice-processing mode change.
 public int getMode(){return MODE_NORMAL;}
 public boolean isValidRingerMode(int mode){return mode>=RINGER_MODE_SILENT&&mode<=RINGER_MODE_NORMAL;}
 public boolean setRingerMode(int mode,boolean hasVibrator){
  if(!isValidRingerMode(mode))throw new IllegalArgumentException("Invalid ringer mode "+mode);
  if(mode==RINGER_MODE_VIBRATE&&!hasVibrator)mode=RINGER_MODE_SILENT;
  synchronized(volumeLock){if(ringerMode==mode)return false;ringerMode=mode;for(int stream=0;stream<volumes.length;stream++)if(isStreamAffectedByRingerMode(stream))applyVolume(stream);return true;}
 }
 public boolean isVolumeFixed(){return false;}
 public void setStreamVolume(int stream,int index,int flags){checkedStream(stream);synchronized(volumeLock){volumes[stream]=Math.max(0,Math.min(15,index));muted[stream]=volumes[stream]==0;applyVolume(stream);}}
 public void setStreamMute(int stream,boolean state){checkedStream(stream);synchronized(volumeLock){muted[stream]=state;applyVolume(stream);}}
 public void adjustStreamVolume(int stream,int direction,int flags){checkedStream(stream);synchronized(volumeLock){
  switch(direction){case ADJUST_SAME:return;case ADJUST_MUTE:muted[stream]=true;break;case ADJUST_UNMUTE:muted[stream]=false;break;case ADJUST_TOGGLE_MUTE:muted[stream]=!muted[stream];break;
   case ADJUST_RAISE:volumes[stream]=Math.min(15,volumes[stream]+1);muted[stream]=false;break;
   case ADJUST_LOWER:volumes[stream]=Math.max(0,volumes[stream]-1);muted[stream]=volumes[stream]==0;break;
   default:throw new IllegalArgumentException("Invalid audio volume adjustment "+direction);}
  applyVolume(stream);
 }}
 public void adjustVolume(int direction,int flags){adjustStreamVolume(STREAM_MUSIC,direction,flags);}
 public void adjustSuggestedStreamVolume(int direction,int suggestedStream,int flags){adjustStreamVolume(suggestedStream==USE_DEFAULT_STREAM_TYPE?STREAM_MUSIC:suggestedStream,direction,flags);}
 static{String library=System.getProperty("klepton.audio");if(library==null)System.loadLibrary("klepton-audio");else System.load(library);}
}
