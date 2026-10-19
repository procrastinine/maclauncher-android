package maclauncher.android.bridge;
/** Private native UI wake. Registration and retirement belong to the host. */
public final class FrameWakeup implements Runnable {
    private final long owner;
    public FrameWakeup(long owner){if(owner==0)throw new IllegalArgumentException("Missing display wake owner");this.owner=owner;}
    @Override public void run(){signal(owner);}
    private static native void signal(long owner);
}
