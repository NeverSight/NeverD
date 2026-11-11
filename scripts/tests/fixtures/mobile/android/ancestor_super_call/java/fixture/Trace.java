package fixture;
public final class Trace {
    public static int created,failAt;
    public static long events;
    public static RuntimeException failure;
    public static Object partial,result;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
