package fixture;
public class Trace {
    public static int events, failAt, initialNumber, afterNumber, number, afterInt;
    public static RuntimeException failure;
    public static Object seed, replacement, value, after;
    public static Base baseReceiver;
    public static Target seen;
    public static void hit(int stage) {
        events = events * 10 + stage;
        if (failAt == stage) throw failure;
    }
}
