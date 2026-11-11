package fixture;
public final class Trace {
    public static int failAt,created;
    public static long events;
    public static Object partial,result;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
