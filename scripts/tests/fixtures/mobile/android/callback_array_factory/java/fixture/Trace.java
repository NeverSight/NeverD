package fixture;
public final class Trace {
    public static int failAt,answer;
    public static long events;
    public static Object fn,keys,result;
    public static boolean flag;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
