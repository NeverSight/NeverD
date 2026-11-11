package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static Object partial, seen;
    public static void hit(int event) { events = events * 10 + event; if (event == fail) throw failure; }
}
