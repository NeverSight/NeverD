package fixture;
public final class Trace {
    public static long events;
    public static int failAt,tail,reads;
    public static RuntimeException failure;
    public static Object seen,result;
    public static void event(int event) { events=events*10+event; if(event==failAt)throw failure; }
}
