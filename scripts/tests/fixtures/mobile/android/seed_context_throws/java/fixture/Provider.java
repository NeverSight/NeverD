package fixture;

public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> T read(T seed, String label, Object... values) throws IllegalArgumentException {
        Trace.calls++;
        Trace.stage = 1;
        Trace.seed = seed;
        Trace.label = label;
        Trace.values = values;
        Object first = values[0];
        Trace.first = first;
        Trace.stage = 2;
        Trace.action.run();
        Trace.stage = 3;
        return (T)Trace.value;
    }
    public static <T> T readFixed(T seed, String label, Object[] values) throws IllegalArgumentException {
        Trace.relays++;
        return read(seed, label, values);
    }
}
