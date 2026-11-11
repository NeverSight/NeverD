package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Receiver<S, V> implements Function {
    public Carrier<S> value;
    public Object last;
    public int calls;
    public Receiver() {}
    public Carrier<S> apply(S input) {
        Trace.events = Trace.events * 10 + 1;
        calls++;
        last = input;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return value;
    }
    public Object invoke(Object input) { return apply((S) input); }
}
