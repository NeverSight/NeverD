package fixture;
public final class Trace {
    public static int events, failAt, capacity, capacitySeen;
    public static RuntimeException failure;
    public static Object input, result, after, key;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == 1 || event == 2) capacity = (capacity ^ 0x5a5a5a5a) + event;
        if (failAt == event) throw failure;
    }
}
