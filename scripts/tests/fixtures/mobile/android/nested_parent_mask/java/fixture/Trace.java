package fixture;
public final class Trace {
    public static Object parent,partial,token,seen,callback;
    public static long events,defaultBits;
    public static int failAt;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
