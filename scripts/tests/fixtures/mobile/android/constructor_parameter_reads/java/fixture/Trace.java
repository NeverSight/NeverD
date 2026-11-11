package fixture;

public final class Trace {
    public static long events;
    public static int calls, failAt, result, baseValue;
    public static Object initialProvider, receiver, input, base, core;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
