package fixture;
public final class Trace {
    public static long events, wide, received;
    public static int failAt, overrides;
    public static RuntimeException failure;
    public static Object first, second, third;
    public static void hit(int n) { events=events*10+n; if(failAt==n)throw failure; }
}
