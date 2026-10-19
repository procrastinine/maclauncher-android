package maclauncher.android.media;

import android.os.*;
import android.system.OsConstants;
import java.nio.ByteBuffer;

/** Native producer transport for AOSP's serialized media-metrics buffer. */
public final class HostMediaMetrics {
    private HostMediaMetrics(){}
    public static int native_submit_bytebuffer(ByteBuffer buffer,int length){
        if(buffer==null||!buffer.isDirect()||length<0||length>buffer.capacity())return -OsConstants.EINVAL;
        IBinder collector=ServiceManager.checkService("media.metrics");
        // Same NO_INIT result as libmediametrics::BaseItem::submitBuffer. An
        // absent optional collector must not turn media operations into errors.
        if(collector==null)return -OsConstants.ENODEV;
        ByteBuffer input=buffer.duplicate();input.clear().limit(length);
        byte[] bytes=new byte[length];input.get(bytes);
        Parcel data=Parcel.obtain();
        try{
            // Original IMediaMetricsService.aidl: oneway submitBuffer(byte[]).
            data.writeInterfaceToken("android.media.IMediaMetricsService");data.writeByteArray(bytes);
            return collector.transact(IBinder.FIRST_CALL_TRANSACTION,data,null,IBinder.FLAG_ONEWAY)?0:-OsConstants.ENOSYS;
        }catch(DeadObjectException gone){return -OsConstants.EPIPE;}
        catch(RemoteException unavailable){return -OsConstants.EIO;}
        finally{data.recycle();}
    }
}
