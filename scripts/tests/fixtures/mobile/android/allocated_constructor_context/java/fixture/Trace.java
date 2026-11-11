package fixture;
public final class Trace {
    public static int failAt;
    public static long events;
    public static Object bucket,context,target,token;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
