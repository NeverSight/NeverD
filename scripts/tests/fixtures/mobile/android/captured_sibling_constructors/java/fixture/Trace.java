package fixture;

public final class Trace {
    public static Object cached;
    public static int events;
    public static int failStage;
    public static RuntimeException failure;

    private Trace() {}

    public static void record(Object value, int stage) {
        events = events * 7 + stage;
        cached = value;
        if (stage == failStage) throw failure;
    }
}
