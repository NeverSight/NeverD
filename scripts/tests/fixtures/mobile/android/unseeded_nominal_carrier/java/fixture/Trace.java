package fixture;
public final class Trace {
    public static long events;public static int failAt;
    public static RuntimeException failure;
    public static Object pair,carrier,seed,extracted,reply;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
