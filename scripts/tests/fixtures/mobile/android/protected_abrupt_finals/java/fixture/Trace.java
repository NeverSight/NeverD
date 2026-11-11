package fixture;
public final class Trace {
    private Trace() { }
    public static int events;
    public static Probe probe;
    public static Object replacement, marker, saved, afterMarker, rejected;
    public static Checked checked;
    public static RuntimeException failure, afterFailure, guardFailure;
    public static Error error;
    public static Object produce(Core self, Object seed) throws Checked {
        events = events * 5 + 1;
        marker = probe.observe(self, 1, seed);
        if (checked != null) throw checked;
        if (failure != null) throw failure;
        if (error != null) throw error;
        saved = replacement;
        events = events * 5 + 2;
        return replacement;
    }
    public static void after(Core self, Object value) {
        events = events * 5 + 3;
        afterMarker = probe.observe(self, 2, value);
        if (afterFailure != null) throw afterFailure;
        events = events * 5 + 4;
    }
    public static void reject(Core self) {
        events = events * 5 + 6;
        rejected = probe.observe(self, 0, null);
    }
}
