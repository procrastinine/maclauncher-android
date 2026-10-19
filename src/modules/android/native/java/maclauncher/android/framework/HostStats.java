package maclauncher.android.framework;

import android.content.Context;
import android.os.Handler;
import android.os.Process;
import com.android.server.SystemService;
import com.android.server.stats.StatsCompanion;
import com.android.server.stats.StatsCompanionService;
import com.android.server.stats.StatsManagerService;

/** Process lifetime for the original StatsD Java service module. Registrations,
 * permissions, native-daemon attachment and retry behavior remain upstream. */
public final class HostStats implements AutoCloseable {
    private final StatsCompanion.Lifecycle lifecycle;
    private final StatsManagerService manager;
    private final StatsCompanionService companion;
    private boolean closed;
    public HostStats(Context context) {
        if(Process.myUid()!=Process.SYSTEM_UID)throw new SecurityException("Statistics services require the Android system owner");
        lifecycle=new StatsCompanion.Lifecycle(context);
        lifecycle.onStart();
        manager=(StatsManagerService)HostUiAccess.get(lifecycle,StatsCompanion.Lifecycle.class,"mStatsManagerService");
        companion=(StatsCompanionService)HostUiAccess.get(lifecycle,StatsCompanion.Lifecycle.class,"mStatsCompanionService");
    }
    public void ready(){
        lifecycle.onBootPhase(SystemService.PHASE_THIRD_PARTY_APPS_CAN_START);
        lifecycle.onBootPhase(SystemService.PHASE_BOOT_COMPLETED);
    }
    @Override public void close(){
        if(closed)return;closed=true;
        HostServices.unregister("statsmanager",manager,"android.os.IStatsManagerService");
        HostServices.unregister("statscompanion",companion,"android.os.IStatsCompanionService");
        Handler handler=(Handler)HostUiAccess.get(companion,StatsCompanionService.class,"mHandler");
        handler.removeCallbacksAndMessages(null);handler.getLooper().quitSafely();
        if(handler.getLooper().getThread()!=Thread.currentThread())try{handler.getLooper().getThread().join();}
        catch(InterruptedException interrupted){Thread.currentThread().interrupt();}
    }
}
