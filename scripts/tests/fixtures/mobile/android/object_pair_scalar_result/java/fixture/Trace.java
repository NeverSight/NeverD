package fixture;
public final class Trace {
    public static int failAt,result;
    public static long events;
    public static Object left,right;
    public static boolean flag;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
