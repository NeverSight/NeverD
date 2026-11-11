package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure,firstFailure,secondFailure,boxFailure;
    public static Object partial;
    public static void hit(int n) { events=events*10+n;if(failAt==n)throw failure; }
}
