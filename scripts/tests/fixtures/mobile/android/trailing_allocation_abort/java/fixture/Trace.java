package fixture;
public final class Trace {
    public static int events, failAt;
    public static Checked seen;
    public static Inner inner;
    public static Outer outer;
    public static Failure aborted;
    public static Object styleSeen;
    public static RuntimeException failure;
    public static Error error;
    public static void step(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
        if (failAt == -event) throw error;
    }
}
