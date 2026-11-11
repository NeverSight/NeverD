package fixture;

public final class Provider<T extends Number> {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> T[] apply(T[] values, Gate<T> gate) {
        Trace.calls++;
        Trace.stage = 1;
        Trace.input = values;
        Trace.gate = gate;
        if (values == null || gate == null) {
            Trace.stage = 5;
            return values;
        }
        Object first = values[0];
        Trace.first = first;
        Trace.stage = 2;
        boolean accepted = gate.accept(values[0]);
        Trace.accepted = accepted;
        Trace.stage = 3;
        Trace.action.run();
        Trace.stage = 4;
        return (T[])Trace.value;
    }
    public static <T> T[] reversed(Gate<T> gate, T[] values) {
        Trace.relays++;
        return apply(values, gate);
    }
}
