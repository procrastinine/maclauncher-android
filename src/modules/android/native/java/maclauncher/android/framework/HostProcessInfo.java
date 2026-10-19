package maclauncher.android.framework;

import android.Manifest;
import android.content.Context;
import android.os.IProcessInfoService;
import android.os.ServiceManager;
import com.android.server.am.ProcessList;
import java.util.Arrays;
import java.util.Objects;

/** The original process-info Binder contract over authenticated host activations. */
public final class HostProcessInfo extends IProcessInfoService.Stub implements AutoCloseable {
    private final Context context;
    private final HostProcessServices processes;
    public HostProcessInfo(Context context,HostProcessServices processes){
        this.context=Objects.requireNonNull(context);this.processes=Objects.requireNonNull(processes);
        HostActivityManager.hostProcessDirectory(processes);
        ServiceManager.addService("processinfo",this);
        try{ServiceManager.addService(Context.ACTIVITY_SERVICE,HostServices.manager().checkService(Context.ACTIVITY_SERVICE));}
        catch(android.os.RemoteException failure){throw failure.rethrowFromSystemServer();}
    }
    @Override public void getProcessStatesFromPids(int[] pids,int[] states){processes.processStates(pids,states);}
    @Override public void getProcessStatesAndOomScoresFromPids(int[] pids,int[] states,int[] scores){
        if(scores!=null)context.enforceCallingOrSelfPermission(Manifest.permission.GET_PROCESS_STATE_AND_OOM_SCORE,"getProcessStatesAndOomScoresForPIDs()");
        Objects.requireNonNull(pids,"pids");
        if(scores!=null&&pids.length!=scores.length)throw new IllegalArgumentException("pids and scores arrays have different lengths");
        processes.processStates(pids,states);
        // Darwin does not maintain Android LMK oom_score_adj. Do not turn a
        // scheduling priority or a foreground indication into an invented score.
        if(scores!=null)Arrays.fill(scores,ProcessList.INVALID_ADJ);
    }
    @Override public void close(){HostServices.unregister("processinfo",this,"android.os.IProcessInfoService");HostActivityManager.hostRetireProcessDirectory(processes);}
}
