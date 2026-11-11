package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static Object seed, late, parent, child;
    public static Adapter adapter;
    public static void hit(int event) {
        events = events * 17 + event;
        if (event == fail) throw failure;
    }
}
