package fixture;
public final class Trace {
    public static long events, left, right;
    public static int shift, failAt;
    public static RuntimeException failure;
    public static Object partial;
    public static void hit(int event) { events=events*10+event; if(event==failAt)throw failure; }
}
