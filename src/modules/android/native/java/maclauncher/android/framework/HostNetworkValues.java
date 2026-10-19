package maclauncher.android.framework;

import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;

/** Converts the existing passive native route snapshot to complete AOSP values. */
public final class HostNetworkValues {
    private HostNetworkValues(){}
    public static NetworkCapabilities capabilities(int state){
        NetworkCapabilities value=new NetworkCapabilities();
        if(state<0||(state&1)==0){value.clearAll();return value;}
        if((state&4)!=0)value.addTransportType(NetworkCapabilities.TRANSPORT_WIFI);
        if((state&8)!=0)value.addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET);
        if((state&16)!=0)value.addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR);
        if((state&1)!=0&&(state&2)==0)value.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        // NWPath observes the permitted default route. Keep Android's normal
        // general-use/trusted/non-VPN defaults and advertise its Internet intent;
        // validation is separate and requires an actual connectivity check.
        value.addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
        value.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED);
        value.addCapability(NetworkCapabilities.NET_CAPABILITY_FOREGROUND);
        value.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VCN_MANAGED);
        return value;
    }
    public static NetworkInfo info(int state){
        int type=(state&4)!=0?ConnectivityManager.TYPE_WIFI:(state&8)!=0?ConnectivityManager.TYPE_ETHERNET:(state&16)!=0?ConnectivityManager.TYPE_MOBILE:-1;
        return info(type,state);
    }
    private static int transport(int type){return switch(type){case ConnectivityManager.TYPE_WIFI->4;case ConnectivityManager.TYPE_ETHERNET->8;case ConnectivityManager.TYPE_MOBILE->16;default->0;};}
    public static boolean supports(int type,int state){int bit=transport(type);return state>=0&&bit!=0&&(((state>>>8)|state)&bit)!=0;}
    public static NetworkInfo info(int type,int state){
        String name=type==ConnectivityManager.TYPE_WIFI?"WIFI":type==ConnectivityManager.TYPE_ETHERNET?"ETHERNET":type==ConnectivityManager.TYPE_MOBILE?"MOBILE":"UNKNOWN";
        NetworkInfo value=new NetworkInfo(type,0,name,"");
        boolean connected=state>=0&&(state&1)!=0&&(state&transport(type))!=0;
        value.setIsAvailable(connected);
        value.setDetailedState(connected?NetworkInfo.DetailedState.CONNECTED:NetworkInfo.DetailedState.DISCONNECTED,null,null);
        return value;
    }
}
