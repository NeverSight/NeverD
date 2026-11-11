package fixture;

public final class Trace {
    public static int events, failAt, overrides;
    public static Object receiver, input, result;
    public static RuntimeException failure;
    public static void step(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
    public static Object parent(Object owner, Object value) {
        receiver = owner;
        input = value;
        step(1);
        step(2);
        step(3);
        return result;
    }
    public static Object override(Object owner, Object value) {
        receiver = owner;
        input = value;
        overrides++;
        events = 9;
        return value;
    }
}
