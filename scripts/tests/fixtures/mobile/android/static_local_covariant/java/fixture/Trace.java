package fixture;
public final class Trace {
    public static long events, wide;
    public static float first, second;
    public static Object seed;
    public static int failAt;
    public static RuntimeException failure;
    public static void hit(int n) { events=events*10+n; if(failAt==n)throw failure; }
}
