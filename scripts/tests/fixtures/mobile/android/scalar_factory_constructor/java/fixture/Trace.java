package fixture;
public final class Trace {
    public static long events;
    public static int failAt,capacitySeen;
    public static boolean flagSeen;
    public static RuntimeException failure;
    public static Object result,partial,box,key,callbackResult;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
