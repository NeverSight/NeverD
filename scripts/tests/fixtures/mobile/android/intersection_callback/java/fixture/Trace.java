package fixture;
public final class Trace {
    public static long events;
    public static int failAt,nodeCalls,getterCalls;
    public static RuntimeException failure;
    public static Object node,callback;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
