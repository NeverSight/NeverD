package fixture;
public final class Trace {
    public static long events,wide;
    public static int failAt,calls;
    public static Object receiver;
    public static Narrow reply;
    public static RuntimeException failure;
    public static void hit(int step) { events=events*10+step;if(step==failAt)throw failure; }
}
