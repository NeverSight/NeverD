package fixture;
public final class Trace {
    public static long events;
    public static int failAt, delta, post;
    public static RuntimeException failure;
    public static Object partial;
    public static void hit(int event) { events=events*10+event; if(failAt==event)throw failure; }
    public static int seed(int value) { hit(1); return value+delta; }
}
