package fixture;
public final class Provider {
    static { Trace.event(2); }
    public static float read(Object seed) {
        float result = Trace.value;
        Trace.seen = seed;
        Trace.reads++;
        Trace.event(3);
        Trace.value = 123.25f;
        return result;
    }
    public static int after() {
        int result = Trace.tail;
        Trace.event(4);
        Trace.tail = 99;
        Trace.value = 456.75f;
        return result;
    }
}
