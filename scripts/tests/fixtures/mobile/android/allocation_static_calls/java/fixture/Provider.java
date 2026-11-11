package fixture;
public final class Provider {
    static {
        Trace.events = Trace.events * 10 + 2;
        if (Trace.providerFailure != null) throw Trace.providerFailure;
    }
    public static Object check(Object input, long word, double bits) {
        Trace.events = Trace.events * 10 + 3;
        Trace.calls++; Trace.seen = input; Trace.stamp = word; Trace.fraction = bits;
        if (Trace.callFailure != null) throw Trace.callFailure;
        return input;
    }
}
