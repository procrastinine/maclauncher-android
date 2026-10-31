package maclauncher.android.framework;

import android.app.usage.NetworkStatsManager;
import android.content.Context;
import android.net.*;
import android.net.connectivity.android.net.netstats.*;
import android.os.*;
import java.io.FileDescriptor;
import dalvik.system.SocketTagger;

/** Mocked Android accounting never reads the Mac's traffic or labels its sockets. */
public final class HostTrafficStats extends INetworkStatsService.Default {
    private static final HostTrafficStats INSTANCE=new HostTrafficStats();
    private static final StatsResult MOCKED=new StatsResult(0,0,0,0);
    private static final SocketTagger TAGGER=new SocketTagger(){
        @Override public void tag(FileDescriptor fd){}
        @Override public void untag(FileDescriptor fd){}
    };
    private static final INetworkStatsSession SESSION=new INetworkStatsSession.Default(){
        @Override public NetworkStats getDeviceSummaryForNetwork(NetworkTemplate template,long start,long end){return empty();}
        @Override public NetworkStats getSummaryForNetwork(NetworkTemplate template,long start,long end){return empty();}
        @Override public NetworkStats getSummaryForAllUid(NetworkTemplate template,long start,long end,boolean tags){return empty();}
        @Override public NetworkStats getTaggedSummaryForAllUid(NetworkTemplate template,long start,long end){return empty();}
        @Override public NetworkStatsHistory getHistoryForNetwork(NetworkTemplate template,int fields){return history();}
        @Override public NetworkStatsHistory getHistoryIntervalForNetwork(NetworkTemplate template,int fields,long start,long end){return history();}
        @Override public NetworkStatsHistory getHistoryForUid(NetworkTemplate template,int uid,int set,int tag,int fields){return history();}
        @Override public NetworkStatsHistory getHistoryIntervalForUid(NetworkTemplate template,int uid,int set,int tag,int fields,long start,long end){return history();}
        @Override public int[] getRelevantUids(){return new int[0];}
    };
    private static NetworkStats empty(){return new NetworkStats(0,0);}
    private static NetworkStatsHistory history(){return new NetworkStatsHistory(3600000);}
    private static boolean installed;
    private HostTrafficStats() {}
    public static synchronized void install(Context context){if(installed)return;HostServices.register(Context.NETWORK_STATS_SERVICE,INSTANCE,"android.net.INetworkStatsService");TrafficStats.init(context);SocketTagger.set(TAGGER);installed=true;}
    public static NetworkStatsManager create(Context context){return new NetworkStatsManager(context,INSTANCE);}
    public static int native_tagSocketFd(FileDescriptor fd,int tag,int uid){return 0;}
    public static int native_untagSocketFd(FileDescriptor fd){return 0;}
    @Override public StatsResult getUidStats(int uid){return MOCKED;}
    @Override public StatsResult getIfaceStats(String iface){return MOCKED;}
    @Override public StatsResult getTotalStats(){return MOCKED;}
    @Override public INetworkStatsSession openSession(){return SESSION;}
    @Override public INetworkStatsSession openSessionForUsageStats(int flags,String callingPackage){return SESSION;}
    @Override public String[] getMobileIfaces(){return new String[0];}
    @Override public TrafficStatsRateLimitCacheConfig getRateLimitCacheConfig(){return new TrafficStatsRateLimitCacheConfig(false,0,0);}
    @Override public void incrementOperationCount(int uid,int tag,int count){}
    @Override public NetworkStats getDataLayerSnapshotForUid(int uid){return empty();}
    @Override public NetworkStats getUidStatsForTransport(int transport){return empty();}
}
