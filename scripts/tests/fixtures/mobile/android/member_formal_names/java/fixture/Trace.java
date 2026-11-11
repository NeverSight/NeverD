package fixture;
public final class Trace {
    public static long events;
    public static int fail, number, reads;
    public static Object receiver, last;
    public static RuntimeException failure;
    public static void hit(int event) { events = events * 10 + event; if (event == fail) throw failure; }
}
