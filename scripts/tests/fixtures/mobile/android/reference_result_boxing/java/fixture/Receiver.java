package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Payload<Object, Object> selected;
    public Receiver() {}
    public Payload<Object, Object> storage() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return selected;
    }
    public Object invoke() { return Carrier.box(storage()); }
}
