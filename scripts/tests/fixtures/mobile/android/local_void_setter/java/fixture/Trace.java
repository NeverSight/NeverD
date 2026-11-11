package fixture;

public final class Trace {
    public static int events, fail;
    public static boolean mutate;
    public static Throwable failure;
    public static Object replacement, seenEntry, seenKey, seenValue, beforeCallback, after;
    private Trace() {}
    public static void hit(int step) {
        events = events * 10 + step;
        if (step == fail) {
            if (failure instanceof Error) throw (Error)failure;
            throw (RuntimeException)failure;
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void callback(Entry entry, Object key, Object value) {
        seenEntry = entry; seenKey = key; seenValue = value;
        beforeCallback = entry.peek();
        hit(4);
        if (mutate) { entry.restore(replacement); hit(5); }
        hit(6);
    }
}
