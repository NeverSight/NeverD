package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static Error error;
    public static Sink sink;
    public static Pair[] pairs, original;
    public static Pair next;
    public static void hit(int event) {
        events = events * 11 + event;
        if (fail == event) throw failure;
        if (fail == -event) throw error;
    }
}
