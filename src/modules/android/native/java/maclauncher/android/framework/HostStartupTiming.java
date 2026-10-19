package maclauncher.android.framework;

/** Optional fixed-stage startup observations; no package or profile data. */
public final class HostStartupTiming {
    private static final boolean ENABLED=Boolean.getBoolean("maclauncher.android.platformStartupTiming");
    private static long began,last;
    private static int count;
    private HostStartupTiming(){}
    public static synchronized void mark(String stage){
        if(!ENABLED||count>=64||!stage.matches("[a-z][a-z0-9-]{0,63}"))return;
        long now=System.nanoTime();if(count++==0)began=last=now;
        System.err.println("[android-platform-startup] "+stage+" elapsedMs="+((now-last)/1_000_000L)+" totalMs="+((now-began)/1_000_000L));
        last=now;
    }
}
