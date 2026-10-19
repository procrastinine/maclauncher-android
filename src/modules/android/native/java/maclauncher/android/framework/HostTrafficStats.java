package maclauncher.android.framework;

import android.app.usage.NetworkStatsManager;
import android.content.Context;
import android.net.*;
import android.net.connectivity.android.net.netstats.*;
import android.os.*;
import java.io.FileDescriptor;

/** macOS has no Android UID/BPF accounting service; preserve its unsupported result. */
public final class HostTrafficStats extends INetworkStatsService.Default {
    private static final HostTrafficStats INSTANCE=new HostTrafficStats();
    private static final StatsResult UNSUPPORTED=new StatsResult(-1,-1,-1,-1);
    private static boolean installed,warned;
    private HostTrafficStats() {}
    public static synchronized void install(Context context){if(installed)return;HostServices.register(Context.NETWORK_STATS_SERVICE,INSTANCE,"android.net.INetworkStatsService");TrafficStats.init(context);TrafficStats.attachSocketTagger();installed=true;}
    public static NetworkStatsManager create(Context context){return new NetworkStatsManager(context,INSTANCE);}
    private static synchronized void unavailable(){if(warned)return;warned=true;System.err.println("[android-host] Android kernel traffic accounting is unavailable");}
    public static int native_tagSocketFd(FileDescriptor fd,int tag,int uid){unavailable();return fd!=null&&fd.valid()?-95:-9;}
    public static int native_untagSocketFd(FileDescriptor fd){unavailable();return fd!=null&&fd.valid()?-95:-9;}
    @Override public StatsResult getUidStats(int uid){unavailable();return UNSUPPORTED;}
    @Override public StatsResult getIfaceStats(String iface){unavailable();return UNSUPPORTED;}
    @Override public StatsResult getTotalStats(){unavailable();return UNSUPPORTED;}
    @Override public String[] getMobileIfaces(){return new String[0];}
    @Override public TrafficStatsRateLimitCacheConfig getRateLimitCacheConfig(){return new TrafficStatsRateLimitCacheConfig(false,0,0);}
    @Override public void incrementOperationCount(int uid,int tag,int count){unavailable();}
    @Override public NetworkStats getDataLayerSnapshotForUid(int uid)throws RemoteException{throw new RemoteException("Android traffic profiles are unavailable");}
    @Override public NetworkStats getUidStatsForTransport(int transport)throws RemoteException{throw new RemoteException("Android traffic profiles are unavailable");}
}
