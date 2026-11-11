package fixture;
public class Trace {
    public static long events;
    public static int fail, index, unboxes;
    public static RuntimeException failure;
    public static Object callback, seen;
    public static void hit(int event) { events = events * 10 + event; if (event == fail) throw failure; }
}
