package fixture;
public final class Trace {
    public static long events;
    public static int failAt,capacitySeen;
    public static boolean flagSeen;
    public static RuntimeException failure;
    public static Runnable action;
    public static Object result,after,key;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
