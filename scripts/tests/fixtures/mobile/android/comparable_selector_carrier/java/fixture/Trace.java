package fixture;
public final class Trace {
    public static long events;
    public static int failAt, callbacks, comparisons, comparison;
    public static RuntimeException failure;
    public static Object seenSeed, seenInput, seenKey;
    public static Box result;
    public static java.util.function.IntConsumer observer;
    public static void hit(int event) {
        events = events * 10 + event;
        if (observer != null) observer.accept(event);
        if (event == failAt) throw failure;
    }
}
