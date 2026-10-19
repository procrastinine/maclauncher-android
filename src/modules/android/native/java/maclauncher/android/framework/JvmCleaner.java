package maclauncher.android.framework;

/** The framework's legacy Cleaner create/clean contract on the real JDK queue. */
public final class JvmCleaner {
    private static final java.lang.ref.Cleaner QUEUE=java.lang.ref.Cleaner.create();
    // A null legacy referent has no collection event; explicit clean still works.
    private static final Object NULL_REFERENT=new Object();
    private final java.lang.ref.Cleaner.Cleanable cleanable;
    private JvmCleaner(Object referent,Runnable action){cleanable=QUEUE.register(referent==null?NULL_REFERENT:referent,action);}
    public static JvmCleaner create(Object referent,Runnable action){return action==null?null:new JvmCleaner(referent,action);}
    public void clean(){cleanable.clean();}
    /** Runnable ownership for the JDK cleaner; the original ART wrapper stays available. */
    public static final class Runner implements Runnable {
        private final JvmCleaner cleaner;
        public Runner(JvmCleaner cleaner){this.cleaner=cleaner;}
        @Override public void run(){cleaner.clean();}
    }
}
