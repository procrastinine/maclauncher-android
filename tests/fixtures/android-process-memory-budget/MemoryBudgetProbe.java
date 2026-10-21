import android.app.ActivityManager;
import android.os.Process;
import java.nio.file.Path;
import maclauncher.android.bridge.Bridge;
import maclauncher.android.framework.HostActivityManager;

public final class MemoryBudgetProbe {
    private static native void bind(boolean source);
    private static native long[] provider();
    private static native long[] procMemory();
    private static native long allocate(long bytes);
    private static native void release(long address,long bytes);
    private static void require(boolean okay,String message){if(!okay)throw new AssertionError(message);}
    private static void nearby(long left,long right,String label){require(Math.abs(left-right)<8L*1024*1024,label+" differs from the existing native memory owner");}
    public static void main(String[] args){
        Path runtime=Path.of(args[0]);
        String core=runtime.resolve("native/libklepton-jvm.dylib").toString();
        System.setProperty("klepton.bridge",core);
        System.load(core);System.load(runtime.resolve("native/libnativebinding.dylib").toString());
        System.load(args[1]);bind(args.length<3||!args[2].equals("runtime"));
        long[] owner=provider();long requested=Long.parseLong(System.getenv("KL_MEM_TOTAL_MB"))*1024*1024;
        require(owner[0]==Math.min(requested,owner[2]),"existing configured budget remains capped by physical memory");
        require(Process.getTotalMemory()==owner[0],"Process total must use the guest budget, not physical host RAM");
        nearby(Process.getFreeMemory(),provider()[1],"Process free memory");
        long[] bridge=new long[4];Bridge.memoryInfo(bridge);
        require(bridge[1]==owner[0],"Bridge total uses the same owner");nearby(bridge[0],provider()[1],"Bridge available memory");
        var info=new ActivityManager.MemoryInfo();HostActivityManager.getMemoryInfo(info);
        require(info.totalMem==owner[0],"ActivityManager total uses the same owner");nearby(info.availMem,Process.getFreeMemory(),"ActivityManager available memory");
        long[] proc=procMemory();require(proc[0]*1024==owner[0],"proc MemTotal uses kB from the same budget");
        require(proc[1]>=0&&proc[1]<=proc[0]&&proc[2]>=0&&proc[2]<=proc[0],"proc free/available remain within the guest budget");
        long before=Process.getFreeMemory(),bytes=32L*1024*1024,address=allocate(bytes);
        require(address!=0,"allocate and touch own private memory");
        try{
            long after=Process.getFreeMemory();require(before-after>=24L*1024*1024,"Process free follows actual process memory use");
            nearby(after,provider()[1],"Process free after allocation");
            Bridge.memoryInfo(bridge);nearby(after,bridge[0],"Bridge after allocation");
        }finally{release(address,bytes);}
        require(Process.getFreeMemory()>before-8L*1024*1024,"released pages restore available headroom");
        System.out.println("PASS original Process/ActivityManager, Bridge and proc share configured budget="+owner[0]+" and live available memory");
    }
}
