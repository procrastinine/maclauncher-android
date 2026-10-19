package maclauncher.android.bridge;
/** Native continuous clock; shares the bridge's existing library ownership. */
public final class Clock {
    private Clock() {}
    static { System.load(System.getProperty("klepton.bridge")); }
    public static native long bootTimeNanos();
}
