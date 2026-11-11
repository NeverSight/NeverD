package fixture;
public final class Trace {
    public static long events, flags;
    public static int failAt, inputs, decoded, callbacks, boxes;
    public static Object receiver, input, output;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
