package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static Object partial;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == fail) throw failure;
    }
    public static void before(Object value, String marker) {
        hit(3);
        if (value == null) throw failure;
    }
}
