package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Object first, second, converter, input, callback, converted, callbackResult, supplied;
    public static String label;
    public static void hit(int event) { events=events*10+event; if(event==failAt)throw failure; }
}
