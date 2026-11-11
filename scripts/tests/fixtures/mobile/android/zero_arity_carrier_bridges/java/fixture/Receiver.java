package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S, V> implements Function {
    public long value;
    public Receiver() {}
    public long apply() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return value;
    }
    public Object invoke() { return CarrierB.wrap(apply()); }
}
