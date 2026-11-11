package fixture;

public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <A,B> Box<A,B> first(Box<A,B> input, A... values) {
        Trace.calls++;
        Trace.entry = 1;
        Trace.input = input;
        Trace.values = values;
        Object saved = Trace.store;
        Trace.saved = saved;
        Trace.stage = 1;
        if (Trace.inspectInput) {
            Trace.inputMarker = input.marker;
            Trace.stage = 2;
        }
        if (Trace.inspectArray) {
            Trace.element = values[0];
            Trace.stage = 3;
        }
        Trace.action.run();
        Trace.stage = 4;
        if (Trace.write) {
            values[Trace.index] = (A)saved;
            Trace.stage = 5;
        }
        return (Box<A,B>)Trace.value;
    }
    @SuppressWarnings("unchecked")
    public static <A,B> Box<A,B> second(Box<A,B> input, B... values) {
        Trace.calls++;
        Trace.entry = 2;
        Trace.input = input;
        Trace.values = values;
        Object saved = Trace.store;
        Trace.saved = saved;
        Trace.stage = 1;
        if (Trace.inspectInput) {
            Trace.inputMarker = input.marker;
            Trace.stage = 2;
        }
        if (Trace.inspectArray) {
            Trace.element = values[0];
            Trace.stage = 3;
        }
        Trace.action.run();
        Trace.stage = 4;
        if (Trace.write) {
            values[Trace.index] = (B)saved;
            Trace.stage = 5;
        }
        return (Box<A,B>)Trace.value;
    }
    public static <A,B> Box<A,B> fixed(Box<A,B> input, A[] values) {
        Trace.forwards++;
        return Provider.first(input, values);
    }
}
