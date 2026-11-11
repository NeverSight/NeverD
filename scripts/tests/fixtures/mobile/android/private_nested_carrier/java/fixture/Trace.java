package fixture;
public final class Trace {
    public static int events, failAt, allocations;
    public static boolean useAlternate;
    public static Atom input, result, alternate, created;
    public static Observer observer;
    public static RuntimeException failure;
    public static Error error;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
        if (failAt == -event) throw error;
    }
}
