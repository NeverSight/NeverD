package fixture;
public final class Trace {
    public static int events, failAt;
    public static Node before, after, seed;
    public static Node.Companion.Action choice, produced;
    public static RuntimeException failure;
    public static Error error;
    public static void step(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
        if (failAt == -event) throw error;
    }
    public static void before(Node value) {
        before = value; seed = value; step(1);
    }
    public static void after(Node value, Node.Companion.Action result) {
        after = value; choice = result; step(3);
    }
}
