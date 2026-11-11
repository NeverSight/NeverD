package fixture;
public final class Trace {
    public static long events;
    public static int failAt,sum,result;
    public static RuntimeException failure;
    public static Object current;
    public static void hit(int event) { events=events*10+event;if(failAt==event)throw failure; }
}
