package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public int seen, calls;
    public Object invoke(Object value) {
        apply(((Carrier) value).unwrap());
        return kotlin.Unit.INSTANCE;
    }
    public final void apply(int value) {
        Trace.events = Trace.events * 10 + 3;
        Trace.first = value;
        seen = value;
        calls++;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
    }
}
