package maclauncher.android.framework;

/** Host I/O participates in the original AOSP thread policy and diagnostics. */
public final class HostStrictMode {
    private HostStrictMode() {}
    private static final java.lang.reflect.Method clear=clearMethod();
    private static java.lang.reflect.Method clearMethod(){try{var method=android.os.StrictMode.class.getDeclaredMethod("clearGatheredViolations");method.setAccessible(true);return method;}catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    public static void hostNoteDiskRead(){android.os.StrictMode.noteDiskRead();}
    public static void hostNoteDiskWrite(){android.os.StrictMode.noteDiskWrite();}
    public static void hostNoteNetwork(){dalvik.system.BlockGuard.getThreadPolicy().onNetwork();}
    public static void hostNoteResourceMismatch(String value){android.os.StrictMode.noteResourceMismatch(value);}
    public static void hostNoteUnbufferedIo(){android.os.StrictMode.noteUnbufferedIO();}
    public static void hostClearThreadPolicy(){
        android.os.StrictMode.setThreadPolicy(android.os.StrictMode.ThreadPolicy.LAX);
        try{clear.invoke(null);}catch(ReflectiveOperationException error){throw new IllegalStateException("StrictMode cleanup failed",error);}
    }
}
