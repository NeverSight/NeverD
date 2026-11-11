package fixture;
public final class Trace {
    public static long events, seenFlags;
    public static int mask, seenBits;
    public static Object seenProducer, seenSeed, seenValue, seenResult, lateValue;
    public static Context context;
    public static Result<?> output;
    public static java.util.function.IntConsumer observer;
    public static void hit(int event) {
        events = events * 10 + event;
        if (observer != null) observer.accept(event);
    }
}
