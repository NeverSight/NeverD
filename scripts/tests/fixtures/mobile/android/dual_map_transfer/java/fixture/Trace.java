package fixture;
public final class Trace {
    public static long events;
    public static int failAt,kind;
    public static RuntimeException failure;
    public static Object first,second,created,after,other;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
