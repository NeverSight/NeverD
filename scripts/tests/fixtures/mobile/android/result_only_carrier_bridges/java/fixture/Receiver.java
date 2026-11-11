package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Receiver<S, V> implements Function {
    public long value;
    public Object last;
    public Receiver() {}
    public long apply(S input) {
        Trace.events = Trace.events * 10 + 1;
        last = input;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return value;
    }
    public Object invoke(Object input) {
        return CarrierB.wrap(apply((S) input));
    }
}
