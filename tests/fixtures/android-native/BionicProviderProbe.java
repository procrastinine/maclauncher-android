public final class BionicProviderProbe {
    private static native long roundTrip(int a, long b, float c, double d, int e, int f, int g, int h, int i, int j);
    public static long callback(long value) { return value * 3 + 7; }
    public static void run() {
        long result = roundTrip(11, 22L, 1.5f, 2.5d, 33, 44, 55, 66, 77, 88);
        if (result != 1204) throw new AssertionError("Bionic JNI result " + result);
    }
}
