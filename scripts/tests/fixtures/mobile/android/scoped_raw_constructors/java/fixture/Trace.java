package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static Object source, initialKey, initialValue, lateValue, published;
    public static void hit(int event) {
        events = events * 11 + event;
        if (event == fail) throw failure;
        if (event == 2) ((Parent) source).value = lateValue;
    }
}
