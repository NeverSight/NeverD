package fixture;
public class Trace {
    public static int events, failAt, initialNumber, afterNumber, afterInt;
    public static RuntimeException failure;
    public static Holder holder;
    public static Link initial, replacement, afterLink;
    public static Object after;
    public static Target seen;
    public static void hit(int stage) {
        events = events * 10 + stage;
        if (failAt == stage) throw failure;
    }
}
