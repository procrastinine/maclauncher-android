package maclauncher.android;
import android.net.Network;
import java.util.function.BiConsumer;
import maclauncher.android.framework.HostConnectivityManager;

/** JNI representation of the shared passive Mac network observer. */
final class ArtNetwork implements HostConnectivityManager.Source {
 private volatile BiConsumer<Long,Long> callback;
 private volatile long active;
 private static native long observe(int operation,long epoch,long snapshot);
 public void listener(BiConsumer<Long,Long> value){callback=value;}
 public boolean continuous(){return true;}
 public long snapshot(){return observe(0,0,0);}
 public long start(long epoch){
  long first=observe(1,epoch,0);active=epoch;
  Thread worker=new Thread(()->{long previous=first;while(active==epoch){long next=observe(2,epoch,previous);if(next==-1||active!=epoch)return;previous=next;callback.accept(epoch,next);}},"Android network observation");
  worker.setDaemon(true);worker.start();return first;
 }
 public void stop(long epoch){if(active==epoch)active=0;observe(3,epoch,0);}
 public void close(){active=0;observe(4,0,0);}
 public void network(Network value,int state){ArtPlatform.network(value==null?0:value.getNetId(),state);}
}
