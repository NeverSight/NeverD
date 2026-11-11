package fixture;
public final class Trace {
    public static int failAt,count;
    public static long events;
    public static boolean ready;
    public static Object context,target,token;
    public static Runnable action,readAction;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
